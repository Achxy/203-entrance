#include "lighting_ble_idf.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <stdatomic.h>
#include <string.h>

#define LB_PREAUTH_MESSAGES 6u
#define LB_AUTH_MESSAGES 40u
#define LB_PREAUTH_BYTES 12288u
#define LB_AUTH_BYTES 24576u
#define LB_BUDGET_MS UINT64_C(10000)
#define LB_ACK_MS UINT64_C(3000)

typedef struct {
    lb_receiver_t rx;
    uint8_t route[16];
    uint16_t handle, mtu;
    bool live, closed_pending, subscribed, authorized, admitted;
    lb_proven_role_t role;
    uint64_t budget_start, idle_deadline, admission_deadline;
    unsigned messages, bytes;
} link_t;
typedef struct {
    uint8_t body[LB_RESPONSE_MAX], route[16];
    size_t size, offset, pending_offset;
    uint32_t id;
    uint8_t operation;
    uint16_t handle;
    uint64_t deadline, ack_deadline;
    bool active, awaiting_ack, close_after;
} response_t;

static link_t links[LB_LINKS];
static response_t response;
static StaticSemaphore_t mutex_buffer;
static SemaphoreHandle_t mutex;
static uint16_t tx_handle;
static unsigned next_link;
static uint8_t own_address_type;
static _Atomic bool running, ready, stopping;
static _Atomic(TaskHandle_t) worker;

/* NimBLE UUID128 initializer expects least-significant byte first. */
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x01,0x00,0x00,0x03,0x03,0x32,0xe4,0xb7,0x7d,0x4d,0xa9,0x0b,0x30,0xe3,0xf5,0x89);
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0x02,0x00,0x00,0x03,0x03,0x32,0xe4,0xb7,0x7d,0x4d,0xa9,0x0b,0x30,0xe3,0xf5,0x89);
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x03,0x00,0x00,0x03,0x03,0x32,0xe4,0xb7,0x7d,0x4d,0xa9,0x0b,0x30,0xe3,0xf5,0x89);

static uint64_t milliseconds(void) { return (uint64_t)esp_timer_get_time() / 1000u; }
static void wake_worker(void)
{
    TaskHandle_t task=worker;
    if(task) xTaskNotifyGive(task);
}
static void lock(void) { xSemaphoreTake(mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(mutex); }
static link_t *by_handle(uint16_t handle)
{
    for (unsigned i=0; i<LB_LINKS; ++i) if (links[i].live && links[i].handle == handle) return &links[i];
    return NULL;
}
static link_t *by_route(const uint8_t route[16])
{
    for (unsigned i=0; i<LB_LINKS; ++i)
        if (links[i].live && !memcmp(links[i].route,route,16)) return &links[i];
    return NULL;
}
static bool admission_expired(const link_t *link,uint64_t now)
{return !link->admitted&&now>=link->admission_deadline;}
static void terminate(uint16_t handle) { (void)ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM); }
static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    if (!ready || stopping || ble_gap_adv_active()) return;
    lock(); unsigned count=0;
    for (unsigned i=0; i<LB_LINKS; ++i) count += (unsigned)(links[i].live || links[i].closed_pending);
    unlock();
    if (count >= LB_LINKS) return;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1; fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields)) return;
    struct ble_hs_adv_fields scan = {0};
    scan.name = (uint8_t *)"203 Lights"; scan.name_len = 10; scan.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&scan)) return;
    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND; parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    parameters.itvl_min = 240; parameters.itvl_max = 320; /* 150–200ms, no active scan. */
    (void)ble_gap_adv_start(own_address_type,NULL,BLE_HS_FOREVER,&parameters,gap_event,NULL);
}

static int access_rx(uint16_t connection, uint16_t attribute, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attribute; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint16_t n = 0; uint8_t packet[LB_ATTRIBUTE_MAX];
    if (ble_hs_mbuf_to_flat(ctxt->om,packet,sizeof(packet),&n) || n < LB_HEADER_BYTES) {
        terminate(connection); return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint64_t now = milliseconds(); bool bad=false, complete=false;
    lock();
    link_t *link = by_handle(connection);
    if (!link || !link->subscribed || admission_expired(link,now)) bad=true;
    else {
        if (now >= link->budget_start + LB_BUDGET_MS) {
            link->budget_start=now; link->messages=link->bytes=0;
        }
        unsigned limit=link->authorized ? LB_AUTH_MESSAGES : LB_PREAUTH_MESSAGES;
        bool first=(packet[2]&LB_START)!=0;
        unsigned byte_limit=link->authorized ? LB_AUTH_BYTES : LB_PREAUTH_BYTES;
        if (link->bytes > byte_limit || n > byte_limit-link->bytes ||
            (first && link->messages >= limit)) bad=true;
        else {
            link->bytes += n;
            if(first) ++link->messages;
            lb_frame_result_t result=lb_receiver_feed(&link->rx,packet,n,now);
            bad=result!=LB_MORE && result!=LB_COMPLETE;
            complete=result==LB_COMPLETE;
        }
    }
    unlock();
    if(bad) { terminate(connection); return BLE_ATT_ERR_UNLIKELY; }
    if(complete) wake_worker();
    return 0;
}

static int access_tx(uint16_t connection,uint16_t attribute,struct ble_gatt_access_ctxt *ctxt,void *arg)
{
    (void)connection;(void)attribute;(void)ctxt;(void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def services[] = {{
    .type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&service_uuid.u,
    .characteristics=(struct ble_gatt_chr_def[]) {
        {.uuid=&rx_uuid.u,.access_cb=access_rx,.flags=BLE_GATT_CHR_F_WRITE},
        {.uuid=&tx_uuid.u,.access_cb=access_tx,.flags=BLE_GATT_CHR_F_INDICATE,.val_handle=&tx_handle},
        {0}
    }
},{0}};

static int gap_event(struct ble_gap_event *event,void *arg)
{
    (void)arg;
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(!event->connect.status) {
            bool accepted=false; uint64_t now=milliseconds();
            lock();
            for(unsigned i=0;i<LB_LINKS;++i) if(!links[i].live && !links[i].closed_pending) {
                link_t *link=&links[i]; memset(link,0,sizeof(*link));
                link->live=true;link->handle=event->connect.conn_handle;link->mtu=23;
                link->budget_start=now;link->idle_deadline=now+LB_MESSAGE_TIMEOUT_MS;
                link->admission_deadline=now+LB_MESSAGE_TIMEOUT_MS;
                esp_fill_random(link->route,16);accepted=true;break;
            }
            unlock();
            if(!accepted) terminate(event->connect.conn_handle);
        }
        advertise();return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        lock(); {
            link_t *link=by_handle(event->disconnect.conn.conn_handle);
            if(link) {
                link->live=false;link->closed_pending=true;
                lb_receiver_release(&link->rx);
                if(response.active && !memcmp(response.route,link->route,16)) memset(&response,0,sizeof(response));
            }
        } unlock();
        wake_worker();
        /* A disconnected slot remains reserved until the application closes
         * its secure session and consumes the closed-route event. */
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        lock(); {
            link_t *link=by_handle(event->subscribe.conn_handle);
            if(link && event->subscribe.attr_handle==tx_handle) link->subscribed=event->subscribe.cur_indicate;
        } unlock(); return 0;
    case BLE_GAP_EVENT_MTU:
        lock(); {
            link_t *link=by_handle(event->mtu.conn_handle);
            if(link) link->mtu=event->mtu.value;
        } unlock(); return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if(event->notify_tx.indication && event->notify_tx.attr_handle==tx_handle) {
            bool failed=false,finished_close=false;
            lock();
            if(response.active && response.awaiting_ack && response.handle==event->notify_tx.conn_handle) {
                if(event->notify_tx.status==BLE_HS_EDONE) {
                    response.offset=response.pending_offset;response.awaiting_ack=false;
                    if(response.offset==response.size) {
                        finished_close=response.close_after;
                        link_t *link=by_handle(response.handle);
                        if(link && !memcmp(link->route,response.route,16)) {
                            lb_receiver_release(&link->rx);
                            if(link->authorized)link->idle_deadline=milliseconds()+UINT64_C(300000);
                        }
                        memset(&response,0,sizeof(response));
                    }
                } else if(event->notify_tx.status!=0) failed=true;
            }
            unlock();
            if(failed||finished_close) terminate(event->notify_tx.conn_handle);
            else wake_worker();
        } return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE: advertise();return 0;
    default:return 0;
    }
}

static void on_sync(void)
{
    if(ble_hs_util_ensure_addr(0) || ble_hs_id_infer_auto(0,&own_address_type)) return;
    ready=true;advertise();
}
static void on_reset(int reason)
{
    (void)reason;ready=false;
    lock();
    for(unsigned i=0;i<LB_LINKS;++i) if(links[i].live) {
        links[i].live=false;links[i].closed_pending=true;lb_receiver_release(&links[i].rx);
    }
    memset(&response,0,sizeof(response));unlock();
    wake_worker();
}
static void host_task(void *unused)
{
    (void)unused;nimble_port_run();nimble_port_freertos_deinit();
}

esp_err_t lb_transport_start(void)
{
    if(running) return ESP_ERR_INVALID_STATE;
    if(!mutex) mutex=xSemaphoreCreateMutexStatic(&mutex_buffer);
    if(!mutex) return ESP_ERR_NO_MEM;
    lock();
    for(unsigned i=0;i<LB_LINKS;++i) if(links[i].closed_pending) { unlock();return ESP_ERR_INVALID_STATE; }
    memset(links,0,sizeof(links));memset(&response,0,sizeof(response));next_link=0;unlock();
    stopping=false;ready=false;
    esp_err_t err=nimble_port_init();
    if(err!=ESP_OK) return err;
    ble_hs_cfg.sync_cb=on_sync;ble_hs_cfg.reset_cb=on_reset;
    ble_svc_gap_init();ble_svc_gatt_init();
    int rc=ble_svc_gap_device_name_set("203 Lights");
    if(!rc) rc=ble_att_set_preferred_mtu(128);
    if(!rc) rc=ble_gatts_count_cfg(services);
    if(!rc) rc=ble_gatts_add_svcs(services);
    if(rc) { nimble_port_deinit();return ESP_FAIL; }
    running=true;nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t lb_transport_stop(void)
{
    if(!running) return ESP_OK;
    stopping=true;ready=false;(void)ble_gap_adv_stop();
    uint16_t handles[LB_LINKS];unsigned count=0;
    lock();for(unsigned i=0;i<LB_LINKS;++i) if(links[i].live) handles[count++]=links[i].handle;unlock();
    for(unsigned i=0;i<count;++i) terminate(handles[i]);
    int rc=nimble_port_stop();
    if(rc) return ESP_FAIL;
    esp_err_t err=nimble_port_deinit();
    if(err!=ESP_OK) return err;
    lock();
    for(unsigned i=0;i<LB_LINKS;++i) if(links[i].live) {
        links[i].live=false;links[i].closed_pending=true;lb_receiver_release(&links[i].rx);
    }
    memset(&response,0,sizeof(response));unlock();
    running=false;
    wake_worker();
    return ESP_OK;
}

bool lb_transport_running(void) { return running && ready && !stopping; }
void lb_transport_set_worker(TaskHandle_t task) { worker=task; }

void lb_transport_poll(uint64_t now)
{
    if(!running) return;
    uint16_t terminate_handles[LB_LINKS];unsigned count=0;
    bool send=false;uint16_t handle=0;uint8_t packet[LB_ATTRIBUTE_MAX];size_t packet_size=0;
    lock();
    for(unsigned i=0;i<LB_LINKS;++i) if(links[i].live &&
        (admission_expired(&links[i],now) || lb_receiver_expired(&links[i].rx,now) ||
         (links[i].rx.phase==LB_IDLE && now>=links[i].idle_deadline))) terminate_handles[count++]=links[i].handle;
    if(response.active) {
        if(now>=response.deadline || (response.awaiting_ack && now>=response.ack_deadline)) {
            bool exists=false;for(unsigned i=0;i<count;++i) exists|=terminate_handles[i]==response.handle;
            if(!exists && count<LB_LINKS) terminate_handles[count++]=response.handle;
        } else if(!response.awaiting_ack) {
            handle=response.handle;
            link_t *link=by_handle(handle);
            uint16_t mtu=link ? link->mtu : 0;
            size_t capacity=mtu>3 ? (size_t)mtu-3 : 0;
            if(capacity>LB_ATTRIBUTE_MAX) capacity=LB_ATTRIBUTE_MAX;
            packet_size=lb_response_fragment(packet,capacity,response.operation,response.id,response.body,
                                               response.size,response.offset,&response.pending_offset);
            if(packet_size) { response.awaiting_ack=true;response.ack_deadline=now+LB_ACK_MS;send=true; }
        }
    }
    unlock();
    for(unsigned i=0;i<count;++i) terminate(terminate_handles[i]);
    if(send) {
        struct os_mbuf *buffer=ble_hs_mbuf_from_flat(packet,(uint16_t)packet_size);
        if(!buffer || ble_gatts_indicate_custom(handle,tx_handle,buffer)) terminate(handle);
    }
    advertise();
}

bool lb_transport_take(lb_event_t *event,uint64_t now)
{
    if(!event || !mutex) return false;
    memset(event,0,sizeof(*event));
    lock();
    for(unsigned i=0;i<LB_LINKS;++i) if(links[i].closed_pending) {
        event->type=LB_DISCONNECTED_EVENT;memcpy(event->route,links[i].route,16);
        memset(&links[i],0,sizeof(links[i]));unlock();return true;
    }
    bool processing=response.active;
    for(unsigned i=0;i<LB_LINKS;++i) processing |= links[i].rx.phase==LB_PROCESSING;
    unsigned first=next_link;
    for(unsigned i=0;i<LB_LINKS;++i)
        if(links[i].live && links[i].role==LB_ROLE_OWNER && links[i].rx.phase==LB_READY) first=i;
    if(!processing) for(unsigned offset=0;offset<LB_LINKS;++offset) {
        unsigned i=(first+offset)%LB_LINKS;
        if(links[i].live && links[i].rx.phase==LB_READY && !admission_expired(&links[i],now) &&
           !lb_receiver_expired(&links[i].rx,now)) {
            link_t *link=&links[i];event->type=LB_REQUEST_EVENT;memcpy(event->route,link->route,16);
            event->id=link->rx.id;event->operation=link->rx.operation;
            event->deadline_ms=link->rx.deadline_ms;event->size=link->rx.size;
            memcpy(event->bytes,link->rx.bytes,event->size);link->rx.phase=LB_PROCESSING;
            next_link=(i+1)%LB_LINKS;
            unlock();return true;
        }
    }
    unlock();return false;
}

esp_err_t lb_transport_reply(const uint8_t route[16],uint32_t id,uint16_t status,
                             const uint8_t *payload,size_t n,uint64_t now)
{
    if(!mutex || !route || n>LB_REQUEST_MAX || (n && !payload) || status<200 || status>599) return ESP_ERR_INVALID_ARG;
    lock();link_t *link=by_route(route);
    if(!link || !link->subscribed || link->rx.phase!=LB_PROCESSING || link->rx.id!=id ||
        admission_expired(link,now) || lb_receiver_expired(&link->rx,now) || response.active) { unlock();return ESP_ERR_INVALID_STATE; }
    response.active=true;memcpy(response.route,route,16);response.handle=link->handle;
    response.id=id;response.operation=link->rx.operation;response.deadline=link->rx.deadline_ms;
    response.close_after=link->rx.operation==LB_CLOSE||status!=200;
    response.size=n+2;response.body[0]=(uint8_t)(status>>8);response.body[1]=(uint8_t)status;
    if(n) memcpy(response.body+2,payload,n);
    unlock();wake_worker();return ESP_OK;
}

void lb_transport_set_authorized(const uint8_t route[16],bool authorized)
{
    if(!mutex || !route) return;
    lock();link_t *link=by_route(route);
    bool expired=link&&authorized&&admission_expired(link,milliseconds());
    uint16_t handle=link?link->handle:0;
    if(link&&!expired) {
        link->authorized=authorized;
        /* Admission is remembered only for transport draining; every command
         * still requires a current application grant. Teardown may clear that
         * grant before its final correlated error indication is acknowledged. */
        if(authorized)link->admitted=true;
    }
    unlock();
    if(expired)terminate(handle);
}
bool lb_transport_set_proven_role(const uint8_t route[16],lb_proven_role_t role)
{
    if(!mutex || !route || role==LB_ROLE_UNPROVEN || role>LB_ROLE_OWNER) return false;
    lock();link_t *link=by_route(route);bool accepted=link!=NULL;uint16_t handle=link?link->handle:0;
    if(link&&admission_expired(link,milliseconds()))accepted=false;
    if(link && link->role!=LB_ROLE_UNPROVEN && link->role!=role) accepted=false;
    if(link && role==LB_ROLE_GUEST)
        for(unsigned i=0;i<LB_LINKS;++i)
            if(&links[i]!=link && links[i].live && links[i].role==LB_ROLE_GUEST) accepted=false;
    if(accepted) link->role=role;
    unlock();
    if(link && !accepted) terminate(handle);
    return accepted;
}
void lb_transport_disconnect(const uint8_t route[16])
{
    if(!mutex || !route) return;
    lock();link_t *link=by_route(route);bool found=link!=NULL;uint16_t handle=found ? link->handle : 0;unlock();
    if(found) terminate(handle);
}
