#include "gateway_service.h"
#include "cJSON.h"
#include "psa/crypto.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t head;
    uint8_t slot[2][LGS_RECORD_BYTES];
    bool have[2], fail_blob, fail_head, fail_after_head;
    unsigned writes;
} store_t;
static lgs_storage_result_t head_read(void *c,uint64_t *v)
{ store_t *s=c;if(!s->head)return LGS_STORAGE_ABSENT;*v=s->head;return LGS_STORAGE_OK; }
static lgs_storage_result_t slot_read(void *c,unsigned index,uint8_t *out,size_t *n)
{ store_t *s=c;if(!s->have[index])return LGS_STORAGE_ABSENT;assert(*n>=LGS_RECORD_BYTES);memcpy(out,s->slot[index],LGS_RECORD_BYTES);*n=LGS_RECORD_BYTES;return LGS_STORAGE_OK; }
static lgs_storage_result_t slot_write(void *c,unsigned index,const uint8_t *bytes,size_t n)
{ store_t *s=c;if(s->fail_blob)return LGS_STORAGE_IO;assert(n==LGS_RECORD_BYTES);memcpy(s->slot[index],bytes,n);s->have[index]=true;++s->writes;return LGS_STORAGE_OK; }
static lgs_storage_result_t head_write(void *c,uint64_t value)
{ store_t *s=c;if(s->fail_head)return LGS_STORAGE_IO;s->head=value;return s->fail_after_head?LGS_STORAGE_IO:LGS_STORAGE_OK; }
static lgs_storage_t storage(store_t *s)
{ return (lgs_storage_t){s,head_read,slot_read,slot_write,head_write}; }
static void profiles(lgs_profile_t p[2])
{ memset(p,0,sizeof(lgs_profile_t)*2);for(unsigned i=0;i<2;++i){memset(p[i].salt,(int)i+1,16);memset(p[i].verifier,(int)i+3,384);} }
static bool hash(const void *p,size_t n,uint8_t out[32])
{ size_t written=0;return psa_hash_compute(PSA_ALG_SHA_256,p,n,out,32,&written)==PSA_SUCCESS&&written==32; }

static void record_tests(void)
{
    lgs_record_t record={.generation=1},decoded={0};profiles(record.profiles);lg_policy_factory(&record.policy,7);
    uint8_t bytes[LGS_RECORD_BYTES],again[LGS_RECORD_BYTES];
    assert(lgs_record_encode(&record,bytes));assert(lgs_record_decode(&decoded,bytes,sizeof(bytes)));
    assert(lgs_record_encode(&decoded,again)&&!memcmp(bytes,again,sizeof(bytes)));
    for(size_t i=0;i<sizeof(bytes);++i) for(unsigned b=0;b<8;++b){bytes[i]^=(uint8_t)(1u<<b);assert(!lgs_record_decode(&decoded,bytes,sizeof(bytes)));bytes[i]^=(uint8_t)(1u<<b);}
    assert(!lgs_record_decode(&decoded,bytes,sizeof(bytes)-1));
    assert(!lgs_name_valid(" \t"));assert(!lgs_name_valid(" "));assert(lgs_name_valid("Desk"));
    assert(lgs_name_valid("机"));assert(!lgs_name_valid("\xc0\x80"));
    assert(!lgs_utf8_valid((const uint8_t *)"\xed\xa0\x80",3));
    store_t store={0};lgs_storage_t io=storage(&store);
    assert(lgs_storage_load(&io,&decoded)==LGS_STORAGE_ABSENT);
    assert(lgs_storage_save(&io,0,&record)==LGS_STORAGE_OK&&store.head==1);
    record.generation=2;store.fail_head=true;
    assert(lgs_storage_save(&io,1,&record)==LGS_STORAGE_IO&&store.head==1);
    assert(lgs_storage_load(&io,&decoded)==LGS_STORAGE_OK&&decoded.generation==1);
    store.fail_head=false;assert(lgs_storage_save(&io,1,&record)==LGS_STORAGE_OK&&store.head==2);
    assert(lgs_storage_save(&io,1,&record)==LGS_STORAGE_CONFLICT);
    store.slot[0][20]^=1;
    assert(lgs_storage_load(&io,&decoded)==LGS_STORAGE_CORRUPT); /* Never fallback to valid generation1. */
    puts("PASS fixed record, 24,576 bit flips, atomic head, orphan and no rollback fallback");
}

typedef struct { bool queued,power;unsigned sends;uint32_t silent_ip,last_ip;char reply[256];uint64_t send_delay_ms; } network_t;
static const uint8_t bulb_mac[6]={0x44,0x55,0x66,0x77,0x88,0x99};
static wiz_error_t peer_resolve(void *ctx,const uint8_t mac[6],wiz_peer_t *peer)
{ (void)ctx;if(memcmp(mac,bulb_mac,5)||mac[5]<0x99||mac[5]>0xa0)return WIZ_NO_PEER;*peer=(wiz_peer_t){.ap_ip=0xc0a80401,.netmask=0xffffff00,.ip=UINT32_C(0xc0a80402)+(uint32_t)(mac[5]-0x99)};memcpy(peer->mac,mac,6);return WIZ_OK; }
static wiz_error_t net_open(void *ctx,const wiz_peer_t *peer)
{ (void)ctx;(void)peer;return WIZ_OK; }
static wiz_error_t net_send(void *ctx,const wiz_peer_t *peer,const uint8_t *bytes,size_t n,wiz_send_guard_t *guard)
{
    network_t *net=ctx;
    wiz_error_t guarded=wiz_send_guard_check(guard,guard->last_now_ms+net->send_delay_ms);
    if(guarded!=WIZ_OK)return guarded;
    ++net->sends;guard->transmitted=true;net->last_ip=peer->ip;assert(n<2048);
    if(net->silent_ip==peer->ip)return WIZ_OK;
    char text[2048];memcpy(text,bytes,n);text[n]=0;
    if(strstr(text,"setPilot")) {
        if(strstr(text,"\"state\":true"))net->power=true;
        if(strstr(text,"\"state\":false"))net->power=false;
        strcpy(net->reply,"{\"method\":\"setPilot\",\"result\":{\"success\":true}}");
    } else if(strstr(text,"getPilot")) {
        snprintf(net->reply,sizeof(net->reply),"{\"method\":\"getPilot\",\"result\":{\"mac\":\"4455667788%02x\",\"state\":%s,\"dimming\":50}}",peer->mac[5],net->power?"true":"false");
    } else if(strstr(text,"getSystemConfig")) {
        strcpy(net->reply,"{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"445566778899\",\"moduleName\":\"ESP24_SHRGB_01\",\"fwVersion\":\"1.36.1\"}}");
    } else if(strstr(text,"getModelConfig")) {
        strcpy(net->reply,"{\"method\":\"getModelConfig\",\"result\":{\"whiteRange\":[2200,6500],\"minDimLevel\":10}}");
    } else return WIZ_IO;
    net->queued=true;return WIZ_OK;
}
static wiz_error_t net_receive(void *ctx,uint8_t *bytes,size_t cap,size_t *n,uint32_t *ip,uint16_t *port)
{
    network_t *net=ctx;if(!net->queued)return WIZ_IDLE;
    *n=strlen(net->reply);assert(*n<=cap);memcpy(bytes,net->reply,*n);*ip=net->last_ip;*port=38899;net->queued=false;return WIZ_OK;
}
static void net_close(void *ctx){network_t *net=ctx;net->queued=false;}
static unsigned guests_closed;
static void close_guests(void *ctx){(void)ctx;++guests_closed;}
static const uint8_t owner_route[16]={1},guest_route[16]={2};
static cJSON *request(lgs_service_t *s,lgs_role_t role,const uint8_t route[16],uint64_t now,const char *input)
{
    uint8_t *reply=NULL;size_t n=0;
    assert(lgs_service_request(s,role,route,now,now+2000,(const uint8_t *)input,strlen(input),&reply,&n));
    assert(n<=LGS_JSON_MAX);cJSON *j=cJSON_ParseWithLength((const char *)reply,n);assert(j);free(reply);return j;
}
static bool success(const cJSON *j){return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"ok"));}
static const char *str(const cJSON *j,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);assert(cJSON_IsString(v));return v->valuestring;}
static void service_tests(void)
{
    store_t store={0};lgs_storage_t disk=storage(&store);network_t net={0};
    wiz_io_t io={&net,peer_resolve,net_open,net_send,net_receive,net_close,NULL};
    lgs_security_hooks_t security={hash,close_guests,NULL};lgs_service_t *s=calloc(1,sizeof(*s));assert(s);
    assert(lgs_service_init(s,7,&disk,&io,&security)==LGS_STORAGE_ABSENT);
    lgs_profile_t p[2];profiles(p);assert(lgs_provision_trusted_admin(s,p)==LGS_STORAGE_OK);
    cJSON *j=request(s,LGS_OWNER,owner_route,10,"{\"v\":1,\"op\":\"room\"}");assert(success(j));cJSON_Delete(j);
    j=request(s,LGS_GUEST,guest_route,11,"{\"v\":1,\"op\":\"room\"}");assert(!success(j));cJSON_Delete(j);
    j=request(s,LGS_OWNER,owner_route,12,"{\"v\":1,\"v\":1,\"op\":\"room\"}");assert(!success(j));cJSON_Delete(j);
    j=request(s,LGS_OWNER,owner_route,13,"{\"v\":1,\"op\":\"room\",\"role\":\"owner\"}");assert(!success(j));cJSON_Delete(j);
    lgs_candidate_t candidate={.identity_verified=true,.capabilities={.features=WIZ_POWER|WIZ_BRIGHTNESS,.brightness_min=10}};memcpy(candidate.mac,bulb_mac,6);
    assert(!lgs_adopt_candidate(s,(lg_identity_t){1,0},0,&candidate,"Desk",false,20));
    assert(lgs_adopt_candidate(s,(lg_identity_t){1,0},0,&candidate,"Desk",true,21));
    j=request(s,LGS_OWNER,owner_route,22,"{\"v\":1,\"op\":\"light\",\"index\":0}");assert(success(j));uint32_t revision=(uint32_t)cJSON_GetObjectItemCaseSensitive(j,"revision")->valuedouble;cJSON_Delete(j);
    const char *enroll="{\"v\":1,\"op\":\"guest.enroll\",\"name\":\"Guest\",\"token\":\"1111111111111111111111111111111111111111111111111111111111111111\"}";
    j=request(s,LGS_GUEST,guest_route,30,enroll);assert(success(j));char grant[21];strcpy(grant,str(j,"grant"));cJSON_Delete(j);
    unsigned writes=store.writes;j=request(s,LGS_GUEST,guest_route,31,enroll);assert(success(j)&&store.writes==writes);cJSON_Delete(j);
    /* Enrollment changed policy; owner must fresh-read the affected light. */
    j=request(s,LGS_OWNER,owner_route,32,"{\"v\":1,\"op\":\"light\",\"index\":0}");assert(success(j));cJSON_Delete(j);
    char command[512];snprintf(command,sizeof(command),"{\"v\":1,\"op\":\"set\",\"id\":\"00000000000000000000000000000001\",\"boot\":\"0000000000000007\",\"policy_epoch\":\"2\",\"sequence\":\"1\",\"expires_ms\":\"1000\",\"targets\":1,\"expected\":[%u,0,0,0,0,0,0,0],\"patch\":{\"power\":true}}",revision);
    j=request(s,LGS_OWNER,owner_route,40,command);assert(success(j)&&!strcmp(str(j,"result"),"queued"));cJSON_Delete(j);
    for(uint64_t now=50;now<200;now+=10)lgs_service_tick(s,now,(wiz_wall_clock_t){0,false});
    j=request(s,LGS_OWNER,owner_route,201,"{\"v\":1,\"op\":\"command\",\"id\":\"00000000000000000000000000000001\"}");assert(success(j)&&!strcmp(str(j,"result"),"verified"));cJSON_Delete(j);
    unsigned sends=net.sends;j=request(s,LGS_OWNER,owner_route,202,command);assert(success(j));cJSON_Delete(j);assert(net.sends==sends);
    j=request(s,LGS_OWNER,owner_route,210,"{\"v\":1,\"op\":\"freeze\",\"targets\":1,\"duration_ms\":60000}");assert(success(j));cJSON_Delete(j);
    j=request(s,LGS_GUEST,guest_route,211,"{\"v\":1,\"op\":\"room\"}");assert(success(j)&&cJSON_GetObjectItemCaseSensitive(j,"frozen")->valuedouble==1);cJSON_Delete(j);
    j=request(s,LGS_GUEST,guest_route,212,"{\"v\":1,\"op\":\"revoke_all\"}");assert(!success(j));cJSON_Delete(j);
    snprintf(command,sizeof(command),"{\"v\":1,\"op\":\"revoke\",\"grant\":\"%s\"}",grant);
    j=request(s,LGS_OWNER,owner_route,220,command);assert(success(j)&&guests_closed);cJSON_Delete(j);
    j=request(s,LGS_GUEST,guest_route,221,enroll);assert(!success(j));cJSON_Delete(j);
    lgs_service_t *restarted=calloc(1,sizeof(*restarted));assert(restarted);
    assert(lgs_service_init(restarted,8,&disk,&io,&security)==LGS_STORAGE_OK);
    j=request(restarted,LGS_GUEST,guest_route,1,enroll);assert(!success(j));cJSON_Delete(j);
    j=request(restarted,LGS_OWNER,owner_route,2,"{\"v\":1,\"op\":\"room\"}");assert(success(j)&&cJSON_GetObjectItemCaseSensitive(j,"frozen")->valuedouble==1);cJSON_Delete(j);
    store.fail_head=true;
    j=request(restarted,LGS_OWNER,owner_route,3,"{\"v\":1,\"op\":\"freeze\",\"targets\":1,\"duration_ms\":0}");assert(!success(j)&&restarted->faulted);cJSON_Delete(j);
    sends=net.sends;lgs_service_tick(restarted,100,(wiz_wall_clock_t){0,false});assert(net.sends==sends);
    free(restarted);free(s);
    puts("PASS provisioning, strict auth/JSON, grants, correlated fake-UDP readback, dedupe, durable revoke/freeze and storage fail-closed");
}
static wiz_error_t peers(void *ctx,wiz_peer_t *out,size_t cap,size_t *n)
{(void)ctx;assert(cap>=1);*n=1;return peer_resolve(NULL,bulb_mac,out);}
static void discovery_route_tests(void)
{
    store_t store={0};lgs_storage_t disk=storage(&store);network_t net={0};
    wiz_io_t io={&net,peer_resolve,net_open,net_send,net_receive,net_close,NULL};
    lgs_security_hooks_t security={hash,close_guests,NULL};lgs_service_t *s=calloc(1,sizeof(*s));assert(s);
    assert(lgs_service_init(s,7,&disk,&io,&security)==LGS_STORAGE_ABSENT);
    lgs_profile_t p[2];profiles(p);assert(lgs_provision_trusted_admin(s,p)==LGS_STORAGE_OK);
    lgs_service_set_discovery(s,peers,NULL);
    cJSON *j=request(s,LGS_OWNER,owner_route,10,"{\"v\":1,\"op\":\"discover\"}");assert(success(j));cJSON_Delete(j);
    for(uint64_t now=20;now<90;now+=10)lgs_service_tick(s,now,(wiz_wall_clock_t){0,false});
    j=request(s,LGS_OWNER,owner_route,91,"{\"v\":1,\"op\":\"candidate\",\"index\":0}");
    assert(success(j)&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"identified"))&&!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"discovering")));cJSON_Delete(j);
    j=request(s,LGS_OWNER,owner_route,92,"{\"v\":1,\"op\":\"adopt\",\"candidate\":0,\"name\":\"Desk\",\"legacy\":false}");assert(!success(j));cJSON_Delete(j);
    j=request(s,LGS_OWNER,owner_route,93,"{\"v\":1,\"op\":\"adopt\",\"candidate\":0,\"name\":\"Desk\",\"legacy\":true}");assert(success(j));cJSON_Delete(j);
    j=request(s,LGS_OWNER,owner_route,94,"{\"v\":1,\"op\":\"light\",\"index\":0}");assert(success(j)&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"legacy_control")));cJSON_Delete(j);
    const char *enroll="{\"v\":1,\"op\":\"guest.enroll\",\"name\":\"Guest\",\"token\":\"2222222222222222222222222222222222222222222222222222222222222222\"}";
    const char *resume="{\"v\":1,\"op\":\"guest.resume\",\"token\":\"2222222222222222222222222222222222222222222222222222222222222222\"}";
    j=request(s,LGS_GUEST,guest_route,100,enroll);assert(success(j));cJSON_Delete(j);
    /* Simulate four lost transport-disconnect notifications. Expiry prevents
     * permanent application-route slot exhaustion; actual teardown must still
     * call close_route promptly rather than waiting for this fallback. */
    for(unsigned i=0;i<4;++i){uint8_t route[16]={(uint8_t)(30+i)};uint64_t now=60101u+(uint64_t)i*60001u;
        j=request(s,LGS_GUEST,route,now,resume);assert(success(j));cJSON_Delete(j);
        assert(lgs_service_route_authorized(s,LGS_GUEST,route,now+1));}
    free(s);puts("PASS read-only candidate discovery, explicit legacy adoption and expired-route recovery");
}
static void owner_preemption_test(void)
{
    store_t store={0};lgs_storage_t disk=storage(&store);network_t net={.silent_ip=0xc0a80402};
    wiz_io_t io={&net,peer_resolve,net_open,net_send,net_receive,net_close,NULL};
    lgs_security_hooks_t security={hash,close_guests,NULL};lgs_service_t *s=calloc(1,sizeof(*s));assert(s);
    assert(lgs_service_init(s,7,&disk,&io,&security)==LGS_STORAGE_ABSENT);
    lgs_profile_t p[2];profiles(p);assert(lgs_provision_trusted_admin(s,p)==LGS_STORAGE_OK);
    lgs_candidate_t candidate={.identity_verified=true,.capabilities={.features=WIZ_POWER,.brightness_min=10}};
    memcpy(candidate.mac,bulb_mac,6);assert(lgs_adopt_candidate(s,(lg_identity_t){1,0},0,&candidate,"Silent",true,1));
    candidate.mac[5]=0x9a;assert(lgs_adopt_candidate(s,(lg_identity_t){1,0},1,&candidate,"Responsive",true,2));
    /* configure_driver rediscovery is conservative; explicitly seed reported
     * fixture capabilities for these two fake bulbs, not actual hardware. */
    s->driver.bulbs[0].capabilities.features=WIZ_POWER;s->driver.bulbs[1].capabilities.features=WIZ_POWER;
    const char *enroll="{\"v\":1,\"op\":\"guest.enroll\",\"name\":\"Guest\",\"token\":\"3333333333333333333333333333333333333333333333333333333333333333\"}";
    cJSON *j=request(s,LGS_GUEST,guest_route,10,enroll);assert(success(j));cJSON_Delete(j);
    lg_identity_t guest={s->record.policy.guests[0].id,s->record.policy.guest_epoch};lg_snapshot_t snapshot;
    assert(lg_snapshot(&s->authority,guest,1,11,&snapshot)==LG_OK);
    lg_command_t guest_command={.boot_id=7,.policy_epoch=snapshot.policy_epoch,.sequence=1,.expires_ms=1800,.targets=1,.intent={.fields=LG_POWER,.power=1}};
    guest_command.id[0]=4;guest_command.digest[0]=4;guest_command.expected_revision[0]=snapshot.lights[0].revision;
    lg_receipt_t receipt;assert(lg_submit(&s->authority,guest,&guest_command,12,&receipt)==LG_OK);
    lgs_service_tick(s,20,(wiz_wall_clock_t){0,false});lgs_service_tick(s,30,(wiz_wall_clock_t){0,false});
    assert(s->driver.active&&net.last_ip==0xc0a80402);
    assert(lg_snapshot(&s->authority,(lg_identity_t){1,0},2,31,&snapshot)==LG_OK);
    lg_command_t owner_command={.boot_id=7,.policy_epoch=snapshot.policy_epoch,.sequence=1,.expires_ms=1800,.targets=2,.intent={.fields=LG_POWER,.power=1}};
    owner_command.id[0]=5;owner_command.digest[0]=5;owner_command.expected_revision[1]=snapshot.lights[1].revision;
    assert(lg_submit(&s->authority,(lg_identity_t){1,0},&owner_command,32,&receipt)==LG_OK);
    lgs_service_tick(s,40,(wiz_wall_clock_t){0,false});lgs_service_tick(s,50,(wiz_wall_clock_t){0,false});
    assert(net.last_ip==0xc0a80403); /* Owner send did not wait for1.8second guest deadline. */
    for(uint64_t now=60;now<100;now+=10)lgs_service_tick(s,now,(wiz_wall_clock_t){0,false});
    assert(lg_command_status(&s->authority,(lg_identity_t){1,0},owner_command.id,101,&receipt)==LG_OK&&receipt.result==LG_VERIFIED);
    assert(lg_command_status(&s->authority,guest,guest_command.id,102,&receipt)==LG_OK&&receipt.result==LG_UNKNOWN);
    free(s);puts("PASS owner preempts an unresponsive guest target without waiting for its deadline");
}
static void renewal_test(void)
{
    store_t store={0};lgs_storage_t disk=storage(&store);network_t net={0};
    wiz_io_t io={&net,peer_resolve,net_open,net_send,net_receive,net_close,NULL};
    lgs_security_hooks_t security={hash,close_guests,NULL};lgs_service_t *first=calloc(1,sizeof(*first));assert(first);
    assert(lgs_service_init(first,7,&disk,&io,&security)==LGS_STORAGE_ABSENT);
    lgs_profile_t p[2];profiles(p);assert(lgs_provision_trusted_admin(first,p)==LGS_STORAGE_OK);
    const char *enroll="{\"v\":1,\"op\":\"guest.enroll\",\"name\":\"Remembered\",\"token\":\"4444444444444444444444444444444444444444444444444444444444444444\"}";
    const char *resume="{\"v\":1,\"op\":\"guest.resume\",\"token\":\"4444444444444444444444444444444444444444444444444444444444444444\"}";
    cJSON *j=request(first,LGS_GUEST,guest_route,10,enroll);assert(success(j)&&!strcmp(str(j,"grant"),"2"));cJSON_Delete(j);
    uint8_t token_hash[32];memcpy(token_hash,first->record.grants[0].token_hash,32);
    lgs_service_t *next=calloc(1,sizeof(*next));assert(next);
    assert(lgs_service_init(next,8,&disk,&io,&security)==LGS_STORAGE_OK);
    j=request(next,LGS_GUEST,guest_route,1,resume);assert(success(j)&&!strcmp(str(j,"grant"),"3"));cJSON_Delete(j);
    assert(next->record.grants[0].id==3&&!memcmp(token_hash,next->record.grants[0].token_hash,32));
    assert(!strcmp(next->record.grants[0].name,"Remembered"));
    j=request(next,LGS_OWNER,owner_route,2,"{\"v\":1,\"op\":\"revoke\",\"grant\":\"2\"}");assert(!success(j)&&!strcmp(str(j,"code"),"conflict"));cJSON_Delete(j);
    /* A head write may succeed physically while its acknowledgement fails.
     * Service faults immediately; reboot must honor the committed revocation. */
    store.fail_after_head=true;
    j=request(next,LGS_OWNER,owner_route,3,"{\"v\":1,\"op\":\"revoke\",\"grant\":\"3\"}");assert(!success(j)&&next->faulted);cJSON_Delete(j);
    store.fail_after_head=false;
    assert(lgs_service_init(first,9,&disk,&io,&security)==LGS_STORAGE_OK);
    j=request(first,LGS_GUEST,guest_route,1,resume);assert(!success(j)&&!strcmp(str(j,"code"),"revoked"));cJSON_Delete(j);
    /* Orphan renewal never becomes active when the generation head did not
     * commit; its higher grant ID cannot erase the retained revoked state. */
    store.slot[store.head&1u][27]^=1;
    assert(lgs_service_init(next,10,&disk,&io,&security)==LGS_STORAGE_CORRUPT&&next->faulted);
    free(first);free(next);puts("PASS same-slot boot renewal, stale revoke conflict, ambiguous commit and revoked/corrupt reboot rejection");
}
static void admission_and_unsent_test(void)
{
    store_t store={0};lgs_storage_t disk=storage(&store);network_t net={0};
    wiz_io_t io={&net,peer_resolve,net_open,net_send,net_receive,net_close,NULL};
    lgs_security_hooks_t security={hash,close_guests,NULL};lgs_service_t *s=calloc(1,sizeof(*s));assert(s);
    assert(lgs_service_init(s,7,&disk,&io,&security)==LGS_STORAGE_ABSENT);
    lgs_profile_t p[2];profiles(p);assert(lgs_provision_trusted_admin(s,p)==LGS_STORAGE_OK);
    lgs_candidate_t candidate={.identity_verified=true,.capabilities={.features=WIZ_POWER,.brightness_min=10}};
    memcpy(candidate.mac,bulb_mac,6);
    for(unsigned i=0;i<LGS_MAX_ENROLLED;++i){candidate.mac[5]=(uint8_t)(0x99+i);assert(lgs_adopt_candidate(s,(lg_identity_t){1,0},(uint8_t)i,&candidate,"Light",true,10+i));}
    candidate.mac[5]=0x9e;assert(!lgs_adopt_candidate(s,(lg_identity_t){1,0},5,&candidate,"Too many",true,20));
    cJSON *j=request(s,LGS_OWNER,owner_route,21,"{\"v\":1,\"op\":\"home_key\",\"key\":\"00112233445566778899aabbccddeeff\"}");assert(success(j));cJSON_Delete(j);
    s->driver.bulbs[0].capabilities.features=WIZ_POWER;
    lg_snapshot_t snapshot;assert(lg_snapshot(&s->authority,(lg_identity_t){1,0},1,22,&snapshot)==LG_OK);
    lg_command_t command={.boot_id=7,.policy_epoch=snapshot.policy_epoch,.sequence=1,.expires_ms=1000,.targets=1,.intent={.fields=LG_POWER,.power=1}};
    command.id[0]=7;command.digest[0]=7;command.expected_revision[0]=snapshot.lights[0].revision;
    lg_receipt_t receipt;assert(lg_submit(&s->authority,(lg_identity_t){1,0},&command,23,&receipt)==LG_OK);
    lgs_service_tick(s,30,(wiz_wall_clock_t){0,false});lgs_service_tick(s,40,(wiz_wall_clock_t){0,false});
    assert(lg_command_status(&s->authority,(lg_identity_t){1,0},command.id,41,&receipt)==LG_OK);
    assert(receipt.result==LG_REJECTED&&receipt.sent==0&&net.sends==0);
    assert(s->bulb_errors[0]==WIZ_CLOCK_REQUIRED);
    j=request(s,LGS_OWNER,owner_route,42,"{\"v\":1,\"op\":\"light\",\"index\":0}");
    assert(success(j)&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"legacy_control")));
    assert(!strcmp(str(j,"error"),"clock_required"));cJSON_Delete(j);
    lgs_service_dispose(s);free(s);puts("PASS five-bulb admission, no-send clock failure and truthful legacy risk after key import");
}

static void send_boundary_test(void)
{
    /* Deterministic stand-in for time spent resolving the AP DHCP lease.
     * No socket is used. Exercise expiry before first send, normal readback,
     * and expiry after a write but before its readback. */
    for(unsigned scenario=0;scenario<3;++scenario){
        store_t store={0};lgs_storage_t disk=storage(&store);network_t net={.send_delay_ms=2};
        wiz_io_t io={&net,peer_resolve,net_open,net_send,net_receive,net_close,NULL};
        lgs_security_hooks_t security={hash,close_guests,NULL};lgs_service_t *s=calloc(1,sizeof(*s));assert(s);
        assert(lgs_service_init(s,7,&disk,&io,&security)==LGS_STORAGE_ABSENT);
        lgs_profile_t p[2];profiles(p);assert(lgs_provision_trusted_admin(s,p)==LGS_STORAGE_OK);
        lgs_candidate_t candidate={.identity_verified=true,.capabilities={.features=WIZ_POWER,.brightness_min=10}};
        memcpy(candidate.mac,bulb_mac,6);assert(lgs_adopt_candidate(s,(lg_identity_t){1,0},0,&candidate,"Desk",true,1));
        lg_snapshot_t snapshot;assert(lg_snapshot(&s->authority,(lg_identity_t){1,0},1,2,&snapshot)==LG_OK);
        lg_command_t command={.boot_id=7,.policy_epoch=snapshot.policy_epoch,.sequence=1,
            .expires_ms=scenario==0?21:scenario==1?1000:31,.targets=1,.intent={.fields=LG_POWER,.power=1}};
        command.id[0]=9;command.digest[0]=9;command.expected_revision[0]=snapshot.lights[0].revision;
        lg_receipt_t receipt;assert(lg_submit(&s->authority,(lg_identity_t){1,0},&command,3,&receipt)==LG_OK);
        lgs_service_tick(s,10,(wiz_wall_clock_t){0,false});
        assert(net.sends==0); /* Scheduling alone never marks SENT. */
        lgs_service_tick(s,20,(wiz_wall_clock_t){0,false});
        assert(!s->faulted&&s->driver.last_now_ms==22&&s->authority.last_now_ms==22);
        assert(lg_command_status(&s->authority,(lg_identity_t){1,0},command.id,23,&receipt)==LG_OK);
        if(scenario==0){
            assert(net.sends==0&&receipt.sent==0&&receipt.result==LG_TIMED_OUT);
            assert(s->bulb_errors[0]==WIZ_TIMEOUT);
        }else{
            assert(net.sends==1&&receipt.sent==1&&receipt.result==LG_SENT);
            lgs_service_tick(s,30,(wiz_wall_clock_t){0,false});
            assert(!s->faulted&&s->driver.last_now_ms==32&&s->authority.last_now_ms==32);
            if(scenario==1){
                assert(net.sends==2);lgs_service_tick(s,40,(wiz_wall_clock_t){0,false});
                assert(lg_command_status(&s->authority,(lg_identity_t){1,0},command.id,41,&receipt)==LG_OK);
                assert(!s->faulted&&receipt.result==LG_VERIFIED);
            }else{
                assert(lg_command_status(&s->authority,(lg_identity_t){1,0},command.id,33,&receipt)==LG_OK);
                assert(net.sends==1&&receipt.sent==1&&receipt.result==LG_UNKNOWN);
                assert(s->bulb_errors[0]==WIZ_TIMEOUT);
            }
        }
        lgs_service_dispose(s);free(s);
    }
    puts("PASS actual-send deadline guard, no expired UDP write/readback and fresh authority timestamps");
}
int main(void)
{ assert(psa_crypto_init()==PSA_SUCCESS);record_tests();service_tests();discovery_route_tests();owner_preemption_test();renewal_test();admission_and_unsent_test();send_boundary_test();printf("Host sizes: record=%zu service=%zu bytes\n",sizeof(lgs_record_t),sizeof(lgs_service_t));return 0; }
