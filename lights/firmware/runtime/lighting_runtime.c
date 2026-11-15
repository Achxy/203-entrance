#include "lighting_runtime.h"
#include "lighting_runtime_recovery.h"
#include "lighting_runtime_boot.h"
#include "lighting_local_time_idf.h"
#include "gateway_nvs.h"
#include "gateway_service.h"
#include "lighting_session_idf.h"
#include "lighting_ble_idf.h"
#include "wiz_esp_idf.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provisional development gates, not measured safe operating limits. Their
 * purpose is refusal until a qualified profile provides sufficient headroom. */
#define LRT_ACTIVATE_FREE (96u*1024u)
#define LRT_SRP_FREE (64u*1024u)
#define LRT_RETAIN_FREE (32u*1024u)
#define LRT_LARGEST (24u*1024u)
#define LRT_STACK_BYTES 10240u
#define LRT_TASK_MARGIN 1024u

typedef struct {
    lgs_service_t service;
    ls_manager_t sessions;
    wiz_esp_idf_t udp;
    llt_idf_t local_time;
    lgs_nvs_t nvs;
    lrq_t relay;
    union { lrq_work_t relay; lb_event_t ble; } input;
    uint64_t application_deadline, next_ble_attempt;
    bool close_guests, session_ready, ble_started, suspended;
} runtime_t;

static lrt_config_t configuration;
static lrt_status_t state;
static runtime_t *runtime;
static TaskHandle_t task;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static _Atomic bool pause_requested;
static _Atomic bool worker_admitted;
static lgs_profile_t pending_profiles[2];
static bool provision_pending, start_called;
static lrr_t recovery;
static lrb_t boot_gate;
static bool stack_sample_clock_set;
static uint64_t stack_sample_at_ms;
typedef struct {
    lrt_failure_reason_t reason;
    uint32_t free_heap, largest_block, required_free, required_largest;
} guard_result_t;

static uint64_t now_ms(void){return (uint64_t)esp_timer_get_time()/1000u;}
static void lock(void){xSemaphoreTake(mutex,portMAX_DELAY);}
static void unlock(void){xSemaphoreGive(mutex);}
static void notify_locked(void){if(task)xTaskNotifyGive(task);}
static void sample_worker_stack(uint64_t now)
{
    lock();
    bool due=!stack_sample_clock_set||(now>=stack_sample_at_ms&&now-stack_sample_at_ms>=1000u);
    if(due){stack_sample_clock_set=true;stack_sample_at_ms=now;}
    unlock();
    if(!due)return;
    /* Pinned ESP-IDF6.1 task.h specifies bytes, unlike standard FreeRTOS words.
     * NULL inspects this worker only; no stored/foreign task handle is used. */
    uint32_t available=(uint32_t)uxTaskGetStackHighWaterMark(NULL);
    lock();
    if(!state.worker_stack_sample_valid||available<state.worker_stack_min_free_bytes)
        state.worker_stack_min_free_bytes=available;
    state.worker_stack_sample_valid=true;
    unlock();
}
static uint64_t boot_integer(void)
{
    uint64_t value=0;
    for(unsigned i=0;i<8;++i)value=(value<<8)|configuration.boot[i];
    return value;
}
static bool heap_at_least(size_t free_bytes,size_t largest)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=free_bytes &&
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=largest;
}
static guard_result_t heap_snapshot(size_t minimum,size_t largest)
{
    guard_result_t g={.free_heap=(uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        .largest_block=(uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        .required_free=(uint32_t)minimum,.required_largest=(uint32_t)largest};
    return g;
}
static void record_failure_locked(lrt_failure_stage_t stage,const guard_result_t *g,esp_err_t error)
{
    state.last_error=error;state.failure_stage=stage;state.failure_reason=g->reason;
    state.failure_at_ms=now_ms();state.failure_free_heap=g->free_heap;
    state.failure_largest_block=g->largest_block;state.required_free_heap=g->required_free;
    state.required_largest_block=g->required_largest;
}
static void report_failure(lrt_failure_stage_t stage,lrt_failure_reason_t reason,
                            esp_err_t error,size_t minimum,size_t largest)
{
    guard_result_t g=heap_snapshot(minimum,largest);g.reason=reason;
    lock();record_failure_locked(stage,&g,error);unlock();
}
static bool peak_try(lrt_peak_t peak,size_t minimum,guard_result_t *g)
{
    size_t largest=LRT_LARGEST;
    if(peak==LRT_PEAK_ACTIVATION&&sizeof(runtime_t)+LRT_TASK_MARGIN>largest)
        largest=sizeof(runtime_t)+LRT_TASK_MARGIN;
    if(!configuration.begin_peak(peak,configuration.context)) {
        *g=heap_snapshot(minimum,largest);g->reason=LRT_REASON_PEAK_BUSY;return false;
    }
    *g=heap_snapshot(minimum,largest);
    if(configuration.resources_available&&!configuration.resources_available(peak,configuration.context))
        g->reason=LRT_REASON_POLICY;
    else if(g->free_heap<minimum)g->reason=LRT_REASON_FREE_HEAP;
    else if(g->largest_block<largest)g->reason=LRT_REASON_LARGEST_BLOCK;
    if(g->reason!=LRT_REASON_NONE)configuration.end_peak(peak,configuration.context);
    return g->reason==LRT_REASON_NONE;
}
static bool peak_begin(lrt_peak_t peak,size_t minimum,lrt_failure_stage_t stage)
{
    guard_result_t g;
    bool okay=peak_try(peak,minimum,&g);
    if(!okay){lock();record_failure_locked(stage,&g,ESP_ERR_NO_MEM);unlock();}
    return okay;
}
static void peak_end(lrt_peak_t peak){configuration.end_peak(peak,configuration.context);}
static bool should_pause(void)
{return pause_requested||configuration.quiescing(configuration.context);}
static bool sha256(const void *data,size_t size,uint8_t out[32])
{
    size_t used=0;
    return psa_hash_compute(PSA_ALG_SHA_256,data,size,out,32,&used)==PSA_SUCCESS&&used==32;
}
static void request_close_guests(void *context){((runtime_t *)context)->close_guests=true;}
static wiz_error_t list_peers(void *context,wiz_peer_t *peers,size_t capacity,size_t *count)
{return wiz_esp_idf_peers(((runtime_t *)context)->udp.ap,peers,capacity,count);}
static bool signature(const char *message,char out[161],void *context)
{(void)context;return configuration.sign_identity(message,out,configuration.context);}

static void close_security_route(runtime_t *r,const uint8_t route[16])
{
    ls_manager_close(&r->sessions,route);
    lgs_service_close_route(&r->service,route);
    lb_transport_set_authorized(route,false);
}
static void close_route(runtime_t *r,const uint8_t route[16],bool notify_remote)
{
    close_security_route(r,route);
    lb_transport_disconnect(route);
    lock();lrq_close(&r->relay,route);unlock();
    if(notify_remote&&configuration.relay_closed)configuration.relay_closed(route,configuration.context);
}
static bool session_exists(runtime_t *r,const uint8_t route[16])
{
    for(unsigned i=0;i<LS_SESSION_COUNT;++i)
        if(r->sessions.sessions[i].handle&&!memcmp(r->sessions.sessions[i].route,route,16))return true;
    return false;
}
static void expire_sessions(runtime_t *r,uint64_t now)
{
    for(unsigned i=0;i<LS_SESSION_COUNT;++i) {
        ls_session_t *s=&r->sessions.sessions[i];
        if(s->handle&&now>=s->expires_at) {
            uint8_t route[16];memcpy(route,s->route,16);close_route(r,route,true);
        }
    }
}
static void close_guest_sessions(runtime_t *r)
{
    if(!r->close_guests)return;
    r->close_guests=false;
    for(unsigned i=0;i<LS_SESSION_COUNT;++i)if(r->sessions.sessions[i].handle&&r->sessions.sessions[i].credential==LS_ROLE_GUEST) {
        uint8_t route[16];memcpy(route,r->sessions.sessions[i].route,16);close_route(r,route,true);
    }
}
static void close_every_session(runtime_t *r)
{
    for(unsigned i=0;i<LS_SESSION_COUNT;++i)if(r->sessions.sessions[i].handle) {
        uint8_t route[16];memcpy(route,r->sessions.sessions[i].route,16);close_route(r,route,true);
    }
    lock();lrq_close_all(&r->relay);unlock();
}

static esp_err_t application(ls_role_t credential,const uint8_t route[16],const uint8_t *request,
                              size_t size,uint8_t **reply,size_t *reply_size,void *context)
{
    runtime_t *r=context;
    if(credential!=LS_ROLE_OWNER&&credential!=LS_ROLE_GUEST)return ESP_ERR_INVALID_STATE;
    lgs_role_t role=credential==LS_ROLE_OWNER?LGS_OWNER:LGS_GUEST;
    uint64_t now=now_ms();
    if(now>=r->application_deadline||should_pause())return ESP_ERR_TIMEOUT;
    bool result=lgs_service_request(&r->service,role,route,now,r->application_deadline,
                                     request,size,reply,reply_size);
    bool authorized=result&&lgs_service_route_authorized(&r->service,role,route,now_ms());
    lb_transport_set_authorized(route,authorized);
    return result?ESP_OK:ESP_FAIL;
}

static bool identity_request(const uint8_t *request,size_t size)
{
    /* This endpoint has one constant request. Only insignificant JSON whitespace
     * may vary, avoiding a pre-auth JSON heap/parser surface. */
    static const uint8_t expected[]="{\"v\":1}";size_t at=0;bool quoted=false;
    if(!request||!size||size>32)return false;
    for(size_t i=0;i<size;++i) {
        if(!quoted&&(request[i]==' '||request[i]=='\t'||request[i]=='\r'||request[i]=='\n'))continue;
        if(at>=sizeof(expected)-1||request[i]!=expected[at++])return false;
        if(request[i]=='"')quoted=!quoted;
    }
    return at==sizeof(expected)-1;
}
static void hex(const uint8_t *data,size_t size,char *out)
{
    static const char h[]="0123456789abcdef";
    for(size_t i=0;i<size;++i){out[i*2]=h[data[i]>>4];out[i*2+1]=h[data[i]&15];}out[size*2]=0;
}
static esp_err_t identity_reply(const uint8_t route[16],uint8_t **out,size_t *size)
{
    char connection[33],boot[17];hex(route,16,connection);hex(configuration.boot,8,boot);
    char *buffer=malloc(256);
    if(!buffer)return ESP_ERR_NO_MEM;
    int n=snprintf(buffer,256,"{\"v\":1,\"protocol\":1,\"connection\":\"%s\",\"boot\":\"%s\","
                              "\"gateway_id\":\"%s\",\"security\":2,\"patch\":1}",connection,boot,configuration.gateway_id);
    if(n<=0||n>=256){free(buffer);return ESP_ERR_INVALID_SIZE;}
    *out=(uint8_t *)buffer;*size=(size_t)n;return ESP_OK;
}

static uint16_t dispatch(runtime_t *r,const uint8_t route[16],uint8_t op,const uint8_t *payload,
                          size_t size,uint64_t deadline,bool bluetooth,uint8_t **reply,size_t *reply_size,
                          lrt_reply_role_t *reply_role)
{
    *reply=NULL;*reply_size=0;*reply_role=LRT_ROLE_OMIT;
    uint64_t now=now_ms();expire_sessions(r,now);
    if(now>=deadline||should_pause()||r->service.faulted) {
        close_security_route(r,route);*reply_role=LRT_ROLE_CLOSED;return 503;
    }
    esp_err_t result=ESP_ERR_INVALID_ARG;
    if(op==LB_IDENTITY) {
        if(identity_request(payload,size))result=identity_reply(route,reply,reply_size);
    } else if(op==LB_CLOSE&&size==0) {
        /* Preserve the transport until its correlated close acknowledgement is
         * delivered. The BLE adapter disconnects after the final indicationACK. */
        close_security_route(r,route);result=ESP_OK;
    } else if(op==LB_SESSION) {
        if(peak_begin(LRT_PEAK_SRP,LRT_SRP_FREE,LRT_FAILURE_SRP_ADMISSION)) {
            result=ls_manager_handshake(&r->sessions,route,payload,size,now,reply,reply_size);
            peak_end(LRT_PEAK_SRP);
        } else result=ESP_ERR_NO_MEM;
    } else if(op==LB_DATA) {
        r->application_deadline=deadline;
        if(peak_begin(LRT_PEAK_PROVISION,LRT_RETAIN_FREE+8192u,LRT_FAILURE_APPLICATION_ADMISSION)) {
            /* Includes AES, the pre-application P256 binding signature, JSON and
             * COW persistence. Do not recursively reserve inside application. */
            result=ls_manager_data(&r->sessions,route,payload,size,now,signature,application,r,reply,reply_size);
            peak_end(LRT_PEAK_PROVISION);
        } else result=ESP_ERR_NO_MEM;
    }
    ls_role_t proven=ls_manager_proven_role(&r->sessions,route);
    if(result==ESP_OK&&proven!=LS_ROLE_NONE) {
        if(bluetooth&&!lb_transport_set_proven_role(route,proven==LS_ROLE_OWNER?LB_ROLE_OWNER:LB_ROLE_GUEST))result=ESP_ERR_INVALID_STATE;
        if(!bluetooth) {
            lock();bool accepted=lrq_set_role(&r->relay,route,proven==LS_ROLE_OWNER?LRQ_OWNER:LRQ_GUEST,now_ms());unlock();
            if(!accepted)result=ESP_ERR_INVALID_STATE;
        }
    }
    if(result==ESP_OK&&now_ms()>=deadline)result=ESP_ERR_TIMEOUT;
    if(result!=ESP_OK) {
        if(*reply){lgs_secure_zero(*reply,*reply_size);free(*reply);*reply=NULL;*reply_size=0;}
        /* The channel still owns a correlated error response. Drop authority
         * now, then let the adapter send/drain that reply before disconnecting. */
        close_security_route(r,route);*reply_role=LRT_ROLE_CLOSED;
        return result==ESP_ERR_NO_MEM?503:result==ESP_ERR_TIMEOUT?504:
               result==ESP_ERR_INVALID_STATE?409:400;
    }
    if(op==LB_SESSION||op==LB_DATA) {
        *reply_role=proven==LS_ROLE_OWNER?LRT_ROLE_OWNER:proven==LS_ROLE_GUEST?LRT_ROLE_GUEST:LRT_ROLE_OMIT;
        if(!session_exists(r,route))lgs_service_close_route(&r->service,route);
    }
    return 200;
}

static void release_reply(uint8_t *reply,size_t size)
{if(reply){lgs_secure_zero(reply,size);free(reply);}}
static void handle_relay(runtime_t *r,uint64_t now)
{
    lock();bool got=lrq_take(&r->relay,&r->input.relay,now);unlock();
    if(!got)return;
    lrq_work_t *work=&r->input.relay;
    uint8_t *reply=NULL;size_t size=0;lrt_reply_role_t role;
    uint16_t status=dispatch(r,work->route,work->operation,work->payload,work->size,
                             work->deadline_ms,false,&reply,&size,&role);
    lock();bool live=lrq_complete(&r->relay,work->route,work->id,now_ms());
    if(status==200)++state.completed;else ++state.rejected;unlock();
    /* An error closes the crypto route, but still return the correlated failure
     * when its original deadline is live. The relay discards stale replies. */
    if(live||((status!=200)&&now_ms()<work->deadline_ms))
        configuration.relay_reply(work->route,work->id,status,reply,size,role,configuration.context);
    if(status!=200||work->operation==LB_CLOSE){lock();lrq_close(&r->relay,work->route);unlock();}
    release_reply(reply,size);memset(&r->input,0,sizeof(r->input));
    close_guest_sessions(r);
}
static void handle_bluetooth(runtime_t *r,uint64_t now)
{
    if(!lb_transport_take(&r->input.ble,now))return;
    lb_event_t *work=&r->input.ble;
    if(work->type==LB_DISCONNECTED_EVENT) {
        ls_manager_close(&r->sessions,work->route);lgs_service_close_route(&r->service,work->route);return;
    }
    ls_role_t proven=ls_manager_proven_role(&r->sessions,work->route);
    lock();bool allowed=lrq_admit_external(&r->relay,proven==LS_ROLE_OWNER?LRQ_OWNER:
                                         proven==LS_ROLE_GUEST?LRQ_GUEST:LRQ_UNKNOWN,now);unlock();
    if(!allowed){close_route(r,work->route,false);lock();++state.rejected;unlock();return;}
    uint8_t *reply=NULL;size_t size=0;lrt_reply_role_t role;
    uint16_t status=dispatch(r,work->route,work->operation,work->bytes,work->size,
                             work->deadline_ms,true,&reply,&size,&role);
    if(lb_transport_reply(work->route,work->id,status,reply,size,now_ms())!=ESP_OK)close_route(r,work->route,false);
    lock();if(status==200)++state.completed;else ++state.rejected;unlock();
    release_reply(reply,size);memset(&r->input,0,sizeof(r->input));close_guest_sessions(r);
}

static void publish_state(runtime_t *r)
{
    unsigned count=0;
    if(r)for(unsigned i=0;i<LS_SESSION_COUNT;++i)if(r->sessions.sessions[i].handle)++count;
    lock();
    state.secure_sessions=count;state.ble_running=lb_transport_running();
    state.free_heap=(uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    state.largest_block=(uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(r){state.storage_healthy=!r->service.faulted;state.active=r->service.provisioned&&!r->service.faulted&&!r->suspended;
        state.local_time=r->local_time.status;}
    unlock();
}
static esp_err_t initialize_sessions(runtime_t *r)
{
    protocomm_security2_params_t owner={.salt=(const char *)r->service.record.profiles[0].salt,.salt_len=16,
        .verifier=(const char *)r->service.record.profiles[0].verifier,.verifier_len=384};
    protocomm_security2_params_t guest={.salt=(const char *)r->service.record.profiles[1].salt,.salt_len=16,
        .verifier=(const char *)r->service.record.profiles[1].verifier,.verifier_len=384};
    esp_err_t e=ls_manager_init(&r->sessions,configuration.gateway_id,configuration.boot,&owner,&guest);
    r->session_ready=e==ESP_OK;return e;
}
static esp_err_t initialize(runtime_t *r)
{
    esp_netif_t *ap=configuration.ap_netif(configuration.context);
    if(!ap)return ESP_ERR_INVALID_STATE;
    lgs_storage_t storage;wiz_io_t io;
    esp_err_t e=lgs_nvs_open(&r->nvs,&storage);
    if(e!=ESP_OK) {
        report_failure(LRT_FAILURE_STORAGE_LOAD,e==ESP_ERR_NO_MEM?LRT_REASON_ALLOCATION:LRT_REASON_STORAGE,e,0,0);
        return e;
    }
    if(wiz_esp_idf_init(&r->udp,ap,NULL,NULL,&io)!=WIZ_OK)return ESP_FAIL;
    lgs_security_hooks_t hooks={sha256,request_close_guests,r};
    lgs_storage_result_t loaded=lgs_service_init(&r->service,boot_integer(),&storage,&io,&hooks);
    if(loaded!=LGS_STORAGE_OK&&loaded!=LGS_STORAGE_ABSENT) {
        esp_err_t error=loaded==LGS_STORAGE_MEMORY?ESP_ERR_NO_MEM:ESP_ERR_INVALID_STATE;
        report_failure(LRT_FAILURE_STORAGE_LOAD,loaded==LGS_STORAGE_MEMORY?LRT_REASON_ALLOCATION:LRT_REASON_STORAGE,error,0,0);
        return error;
    }
    if(loaded==LGS_STORAGE_ABSENT) {
        lock();bool provision=provision_pending;unlock();
        if(!provision)return ESP_ERR_NOT_FOUND;
        loaded=lgs_provision_trusted_admin(&r->service,pending_profiles);
        esp_err_t provision_result=loaded==LGS_STORAGE_OK?ESP_OK:
                                   loaded==LGS_STORAGE_MEMORY?ESP_ERR_NO_MEM:ESP_FAIL;
        lock();provision_pending=false;state.provisioning=false;
        state.provision_result=provision_result;
        lgs_secure_zero(pending_profiles,sizeof(pending_profiles));unlock();
        if(provision_result!=ESP_OK) {
            report_failure(LRT_FAILURE_PROVISION,loaded==LGS_STORAGE_MEMORY?LRT_REASON_ALLOCATION:LRT_REASON_STORAGE,provision_result,0,0);
            return provision_result;
        }
    } else {
        lock();
        if(provision_pending){provision_pending=false;state.provisioning=false;state.provision_result=ESP_ERR_INVALID_STATE;
            lgs_secure_zero(pending_profiles,sizeof(pending_profiles));}
        unlock();
    }
    lock();state.configuration_present=true;unlock();
    lgs_service_set_discovery(&r->service,list_peers,r);
    e=initialize_sessions(r);
    if(e==ESP_OK)llt_idf_init(&r->local_time,ap,configuration.wall_clock,configuration.context);
    return e;
}
static void suspend(runtime_t *r)
{
    r->suspended=true;llt_idf_stop(&r->local_time);close_every_session(r);
    if(r->ble_started) {
        esp_err_t error=lb_transport_stop();
        if(error==ESP_OK)r->ble_started=false;
        else {lock();state.last_error=error;unlock();}
    }
    wiz_completion_t completion;(void)wiz_driver_cancel(&r->service.driver,&completion);
    if(r->service.driver_io.close)r->service.driver_io.close(r->service.driver_io.context);
    while(lb_transport_take(&r->input.ble,now_ms()))
        if(r->input.ble.type==LB_DISCONNECTED_EVENT) {
            ls_manager_close(&r->sessions,r->input.ble.route);lgs_service_close_route(&r->service,r->input.ble.route);
        }
    lock();state.active=false;state.local_time=r->local_time.status;state.quiesced=!r->ble_started;unlock();
}
static void try_start_ble(runtime_t *r,uint64_t now)
{
    if(r->ble_started||now<r->next_ble_attempt)return;
    r->next_ble_attempt=now+30000;
    if(!peak_begin(LRT_PEAK_BLE_START,LRT_SRP_FREE,LRT_FAILURE_BLE_ADMISSION)) {
        lock();state.resource_blocked=true;unlock();return;
    }
    esp_err_t result=lb_transport_start();
    r->ble_started=result==ESP_OK;
    guard_result_t g=heap_snapshot(LRT_RETAIN_FREE,LRT_LARGEST);
    bool healthy=g.free_heap>=LRT_RETAIN_FREE&&g.largest_block>=LRT_LARGEST;
    if(r->ble_started&&!healthy) {
        g.reason=g.free_heap<LRT_RETAIN_FREE?LRT_REASON_FREE_HEAP:LRT_REASON_LARGEST_BLOCK;
        esp_err_t stopped=lb_transport_stop();r->ble_started=stopped!=ESP_OK;result=ESP_ERR_NO_MEM;
    } else if(result!=ESP_OK)g.reason=result==ESP_ERR_NO_MEM?LRT_REASON_ALLOCATION:LRT_REASON_PLATFORM;
    if(result!=ESP_OK){lock();record_failure_locked(LRT_FAILURE_BLE_START,&g,result);unlock();}
    peak_end(LRT_PEAK_BLE_START);
    lock();if(result!=ESP_OK)state.last_error=result;state.resource_blocked=!healthy||result==ESP_ERR_NO_MEM;unlock();
}

static void worker(void *unused)
{
    (void)unused;
    /* The creator reserves peak allocation before allocating this task. Other
     * notifications (such as quiesce) must not bypass release of that lease. */
    while(!worker_admitted)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));
    esp_err_t error=ESP_ERR_NO_MEM;runtime_t *r=NULL;
    if(peak_begin(LRT_PEAK_ACTIVATION,LRT_ACTIVATE_FREE,LRT_FAILURE_ACTIVATION_ADMISSION)) {
        r=calloc(1,sizeof(*r));
        if(r) {
            lrq_init(&r->relay,configuration.boot);
            error=initialize(r);
            if(error==ESP_OK&&!heap_at_least(LRT_RETAIN_FREE,LRT_LARGEST)) {
                guard_result_t g=heap_snapshot(LRT_RETAIN_FREE,LRT_LARGEST);
                g.reason=g.free_heap<LRT_RETAIN_FREE?LRT_REASON_FREE_HEAP:LRT_REASON_LARGEST_BLOCK;
                lock();record_failure_locked(LRT_FAILURE_RETAINED_HEAP,&g,ESP_ERR_NO_MEM);unlock();
                error=ESP_ERR_NO_MEM;
            }
        } else report_failure(LRT_FAILURE_CONTEXT_ALLOCATION,LRT_REASON_ALLOCATION,ESP_ERR_NO_MEM,sizeof(*r),sizeof(*r));
        peak_end(LRT_PEAK_ACTIVATION);
    }
    if(error!=ESP_OK) {
        if(r){llt_idf_stop(&r->local_time);if(r->session_ready)ls_manager_dispose(&r->sessions);
            lgs_nvs_close(&r->nvs);lgs_secure_zero(r,sizeof(*r));free(r);}
        sample_worker_stack(now_ms());
        lock();state.last_error=error;state.active=false;state.resource_blocked=error==ESP_ERR_NO_MEM;
        if(error!=ESP_ERR_NO_MEM)state.storage_healthy=false;
        if(provision_pending){state.provision_result=error;state.provisioning=false;provision_pending=false;lgs_secure_zero(pending_profiles,sizeof(pending_profiles));}
        state.worker_running=false;task=NULL;
        if(error==ESP_ERR_NO_MEM&&state.configuration_present&&state.storage_healthy)lrr_failure(&recovery,now_ms());
        unlock();vTaskDelete(NULL);return;
    }
    lock();runtime=r;state.configuration_present=true;state.active=true;state.storage_healthy=true;
    state.resource_blocked=false;lrr_success(&recovery);unlock();
    lb_transport_set_worker(xTaskGetCurrentTaskHandle());
    bool relay_turn=true;
    for(;;) {
        uint64_t now=now_ms();
        expire_sessions(r,now);close_guest_sessions(r);
        lock();lrq_expire(&r->relay,now);unlock();
        uint8_t closed[16];
        for(;;){lock();bool got=lrq_take_closed(&r->relay,closed);unlock();if(!got)break;
            ls_manager_close(&r->sessions,closed);lgs_service_close_route(&r->service,closed);
            if(configuration.relay_closed)configuration.relay_closed(closed,configuration.context);}
        if(should_pause()||r->service.faulted) {
            if(!r->suspended||r->ble_started)suspend(r);
        } else {
            if(r->suspended){r->suspended=false;lock();state.quiesced=false;unlock();r->next_ble_attempt=0;}
            try_start_ble(r,now);
            lb_transport_poll(now);
            if(relay_turn){handle_relay(r,now);handle_bluetooth(r,now_ms());}
            else {handle_bluetooth(r,now);handle_relay(r,now_ms());}
            relay_turn=!relay_turn;
            llt_clock_t sample={0};
            bool sampled=configuration.wall_clock&&configuration.wall_clock(&sample,configuration.context);
            wiz_wall_clock_t clock={.trusted=sampled&&sample.trusted&&sample.seconds>0,
                                    .unix_seconds=sample.seconds>0?(uint64_t)sample.seconds:0};
            guard_result_t poll_guard;
            if(peak_try(LRT_PEAK_PROVISION,LRT_RETAIN_FREE+8192u,&poll_guard)) {
                lgs_service_tick(&r->service,now_ms(),clock);
                if(should_pause()||r->service.faulted)llt_idf_stop(&r->local_time);
                else llt_idf_poll(&r->local_time,now_ms());
                peak_end(LRT_PEAK_PROVISION);
            }
        }
        sample_worker_stack(now_ms());
        publish_state(r);
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));
    }
}

static bool recovery_eligible_locked(void)
{
    return !boot_gate.quarantined&&start_called&&state.prepared&&state.configuration_present&&state.storage_healthy&&
           !state.active&&!state.worker_running&&!task&&!runtime&&!state.provisioning&&!provision_pending&&
           !pause_requested&&state.resource_blocked&&state.last_error==ESP_ERR_NO_MEM;
}
static bool admission_lock(bool automatic)
{return xSemaphoreTake(mutex,automatic?0:portMAX_DELAY)==pdTRUE;}
static esp_err_t create_worker(bool automatic,uint64_t requested_now)
{
    if(!admission_lock(automatic))return ESP_ERR_TIMEOUT;
    bool blocked=boot_gate.quarantined||pause_requested;
    bool exists=task!=NULL;
    bool eligible=!automatic||lrr_due(&recovery,requested_now,recovery_eligible_locked());
    unlock();
    if(blocked)return ESP_ERR_INVALID_STATE;
    if(exists)return ESP_OK;
    if(!eligible||(automatic&&should_pause()))return ESP_ERR_INVALID_STATE;
    /* Keep the worker's activation allowance after its stack and task control
     * allocation. Refusing only inside worker() would already consume 10 KiB. */
    guard_result_t g;
    if(!peak_try(LRT_PEAK_ACTIVATION,LRT_ACTIVATE_FREE+LRT_STACK_BYTES+LRT_TASK_MARGIN,&g)) {
        esp_err_t refusal=ESP_ERR_NO_MEM;
        if(admission_lock(automatic)) {
            if(task)refusal=ESP_OK;
            else if(boot_gate.quarantined||pause_requested)refusal=ESP_ERR_INVALID_STATE;
            else if(!automatic||lrr_take(&recovery,requested_now,recovery_eligible_locked())) {
                record_failure_locked(LRT_FAILURE_WORKER_ADMISSION,&g,ESP_ERR_NO_MEM);
                state.resource_blocked=true;
                if(state.configuration_present&&state.storage_healthy)lrr_failure(&recovery,now_ms());
            }
            unlock();
        }
        return refusal;
    }
    if(automatic&&should_pause()){peak_end(LRT_PEAK_ACTIVATION);return ESP_ERR_INVALID_STATE;}
    if(!admission_lock(automatic)){peak_end(LRT_PEAK_ACTIVATION);return ESP_ERR_TIMEOUT;}
    if(boot_gate.quarantined||pause_requested) {
        unlock();peak_end(LRT_PEAK_ACTIVATION);return ESP_ERR_INVALID_STATE;
    }
    if(task){unlock();peak_end(LRT_PEAK_ACTIVATION);return ESP_OK;}
    if(automatic&&(pause_requested||!lrr_take(&recovery,requested_now,recovery_eligible_locked()))) {
        unlock();peak_end(LRT_PEAK_ACTIVATION);return ESP_ERR_INVALID_STATE;
    }
    recovery.pending=false;recovery.next_ms=0;
    worker_admitted=false;
    state.worker_running=true;
    esp_err_t result=ESP_OK;
    if(xTaskCreate(worker,"lights",LRT_STACK_BYTES,NULL,tskIDLE_PRIORITY,&task)!=pdPASS) {
        task=NULL;state.worker_running=false;state.resource_blocked=true;
        g=heap_snapshot(LRT_STACK_BYTES+LRT_TASK_MARGIN,LRT_STACK_BYTES);
        g.reason=LRT_REASON_ALLOCATION;
        record_failure_locked(LRT_FAILURE_WORKER_ALLOCATION,&g,ESP_ERR_NO_MEM);
        if(state.configuration_present&&state.storage_healthy)lrr_failure(&recovery,now_ms());
        result=ESP_ERR_NO_MEM;
    }
    unlock();
    peak_end(LRT_PEAK_ACTIVATION);
    if(result==ESP_OK) {
        worker_admitted=true;
        if(admission_lock(automatic)){notify_locked();unlock();}
    }
    return result;
}
esp_err_t lighting_runtime_prepare(const lrt_config_t *config)
{
    if(!config||!config->ap_netif||!config->sign_identity||!config->quiescing||
        !config->begin_peak||!config->end_peak||!config->relay_reply)return ESP_ERR_INVALID_ARG;
    uint8_t zero[32]={0};char checked[LS_BINDING_CONTEXT_MAX];
    if(!memchr(config->gateway_id,0,sizeof(config->gateway_id))||
        !ls_binding_context(checked,config->gateway_id,config->boot,zero,zero,LS_ROLE_OWNER))return ESP_ERR_INVALID_ARG;
    bool nonzero=false;for(unsigned i=0;i<8;++i)nonzero|=config->boot[i]!=0;
    if(!nonzero)return ESP_ERR_INVALID_ARG;
    if(!mutex)mutex=xSemaphoreCreateMutexStatic(&mutex_storage);
    lock();if(state.prepared){unlock();return ESP_ERR_INVALID_STATE;}
    esp_reset_reason_t reason=esp_reset_reason();
    /* Unknown/future reasons fail closed. These three expected resets retain
     * ordinary startup; panic, watchdog and brownout stop lighting only. */
    lrb_init(&boot_gate,reason==ESP_RST_POWERON||reason==ESP_RST_SW||reason==ESP_RST_DEEPSLEEP);
    configuration=*config;state.reset_reason=(int)reason;state.prepared=true;
    state.storage_healthy=true;state.provision_result=ESP_ERR_NOT_FOUND;unlock();
    nvs_handle_t handle=0;esp_err_t result=nvs_open_from_partition("state","lights_v1",NVS_READONLY,&handle);
    if(result==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    if(result!=ESP_OK){lock();state.storage_healthy=false;state.last_error=result;unlock();return result;}
    uint64_t head=0;result=nvs_get_u64(handle,"head",&head);
    bool had_head=result==ESP_OK;
    if(result==ESP_OK) {
        size_t size=0;
        result=head?nvs_get_blob(handle,(head&1u)?"record1":"record0",NULL,&size):ESP_ERR_INVALID_STATE;
        if(result==ESP_ERR_NVS_NOT_FOUND)result=ESP_ERR_INVALID_STATE;
        if(result==ESP_OK&&size!=LGS_RECORD_BYTES)result=ESP_ERR_INVALID_SIZE;
    }
    nvs_close(handle);
    lock();
    if(result==ESP_OK){state.configuration_present=true;state.provision_result=ESP_OK;}
    else if(had_head||result!=ESP_ERR_NVS_NOT_FOUND){state.storage_healthy=false;state.last_error=result;}
    unlock();return !had_head&&result==ESP_ERR_NVS_NOT_FOUND?ESP_OK:result;
}
esp_err_t lighting_runtime_start(void)
{
    if(!mutex)return ESP_ERR_INVALID_STATE;
    lock();
    bool valid=state.prepared&&state.storage_healthy&&!boot_gate.quarantined;
    bool configured=state.configuration_present;
    if(state.prepared)start_called=true;
    unlock();
    return !valid?ESP_ERR_INVALID_STATE:!configured?ESP_OK:create_worker(false,now_ms());
}
void lighting_runtime_status(lrt_status_t *out)
{
    if(!out)return;
    if(!mutex){memset(out,0,sizeof(*out));return;}
    lock();*out=state;out->quarantined=boot_gate.quarantined;
    out->quarantine_retry_used=boot_gate.retry_used;out->recovery_pending=recovery.pending;
    out->recovery_attempts=recovery.attempts;out->recovery_failures=recovery.failures;
    out->recovery_at_ms=recovery.next_ms;unlock();
    out->free_heap=(uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    out->largest_block=(uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
}
void lighting_runtime_poll_recovery(uint64_t now)
{if(mutex)(void)create_worker(true,now);}
esp_err_t lighting_runtime_provision_trusted_admin(const lgs_profile_t profiles[2])
{
    if(!mutex||!profiles)return ESP_ERR_INVALID_ARG;
    for(unsigned p=0;p<2;++p) {
        unsigned salt=0,verifier=0;
        for(unsigned i=0;i<16;++i)salt|=profiles[p].salt[i];
        for(unsigned i=0;i<384;++i)verifier|=profiles[p].verifier[i];
        if(!salt||!verifier)return ESP_ERR_INVALID_ARG;
    }
    lock();
    if(!state.prepared||!state.storage_healthy||boot_gate.quarantined||pause_requested||
       state.configuration_present||state.provisioning||task){unlock();return ESP_ERR_INVALID_STATE;}
    unlock();
    if(should_pause())return ESP_ERR_INVALID_STATE;
    lock();
    if(!state.storage_healthy||boot_gate.quarantined||pause_requested||
       state.configuration_present||state.provisioning||task){unlock();return ESP_ERR_INVALID_STATE;}
    memcpy(pending_profiles,profiles,sizeof(pending_profiles));provision_pending=true;
    state.provisioning=true;state.provision_result=ESP_ERR_NOT_FINISHED;
    unlock();
    esp_err_t result=create_worker(false,now_ms());
    lock();
    if(result!=ESP_OK){provision_pending=false;state.provisioning=false;state.provision_result=result;lgs_secure_zero(pending_profiles,sizeof(pending_profiles));}
    unlock();return result;
}
esp_err_t lighting_runtime_retry_trusted_admin(void)
{
    if(!mutex)return ESP_ERR_INVALID_STATE;
    lock();bool prepared=state.prepared;unlock();
    if(!prepared||should_pause())return ESP_ERR_INVALID_STATE;
    lock();
    if(!start_called||!state.storage_healthy||pause_requested||task||runtime||
       state.provisioning||provision_pending||!lrb_release_once(&boot_gate)) {
        unlock();return ESP_ERR_INVALID_STATE;
    }
    bool configured=state.configuration_present;
    unlock();
    if(!configured)return ESP_OK;
    esp_err_t result=create_worker(false,now_ms());
    if(result==ESP_OK||result==ESP_ERR_NO_MEM)return ESP_OK;
    /* A quiesce can race the first eligibility snapshot. Do not consume the
     * administrator's retry when no worker was admitted. */
    lock();
    if(!task&&!runtime)lrb_cancel_release(&boot_gate);
    else result=ESP_OK;
    unlock();return result;
}
lrq_result_t lighting_runtime_submit_relay(const lrq_request_t *request)
{
    if(!mutex||!request)return LRQ_ARGUMENT;
    lock();
    if(!runtime||!state.active||pause_requested){unlock();return LRQ_CAPACITY;}
    lrq_result_t result=lrq_submit(&runtime->relay,request,now_ms());
    if(result!=LRQ_OK){++state.rejected;lrq_close(&runtime->relay,request->route);}
    notify_locked();unlock();return result;
}
void lighting_runtime_close_relay(const uint8_t route[16])
{if(!mutex||!route)return;lock();if(runtime)lrq_close(&runtime->relay,route);notify_locked();unlock();}
void lighting_runtime_relay_disconnected(void)
{if(!mutex)return;lock();if(runtime)lrq_close_all(&runtime->relay);notify_locked();unlock();}
bool lighting_runtime_quiesce(uint32_t timeout_ms)
{
    if(timeout_ms>7000)return false;
    pause_requested=true;
    if(!mutex)return true;
    lock();notify_locked();bool absent=task==NULL;unlock();if(absent)return true;
    uint64_t deadline=now_ms()+timeout_ms;
    do {lrt_status_t snapshot;lighting_runtime_status(&snapshot);if(snapshot.quiesced)return true;
        vTaskDelay(pdMS_TO_TICKS(20));}while(now_ms()<deadline);
    return false;
}
void lighting_runtime_resume(void)
{pause_requested=false;if(mutex){lock();notify_locked();unlock();}}

const char *lighting_runtime_failure_stage_name(lrt_failure_stage_t stage)
{
    static const char *const names[]={"none","worker_admission","worker_allocation",
        "activation_admission","context_allocation","storage_load","provision","retained_heap",
        "ble_admission","ble_start","srp_admission","application_admission"};
    return (unsigned)stage<sizeof(names)/sizeof(names[0])?names[stage]:"unknown";
}
const char *lighting_runtime_failure_reason_name(lrt_failure_reason_t reason)
{
    static const char *const names[]={"none","peak_busy","policy","free_heap","largest_block",
        "allocation","storage","platform"};
    return (unsigned)reason<sizeof(names)/sizeof(names[0])?names[reason]:"unknown";
}
