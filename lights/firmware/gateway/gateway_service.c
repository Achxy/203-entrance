#include "gateway_service.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const lg_identity_t owner = {LG_OWNER_SESSION, 0};
static unsigned enrolled_count(const lgs_record_t *r)
{ unsigned count=0;for(unsigned i=0;i<LG_MAX_LIGHTS;++i)if(r->bulbs[i].present)++count;return count; }
static bool constant_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < n; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}
static void hex(const uint8_t *p, size_t n, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[2*i] = h[p[i] >> 4]; out[2*i+1] = h[p[i] & 15]; }
    out[2*n] = 0;
}
static bool unhex(const char *s, uint8_t *out, size_t n)
{
    if (!s || strlen(s) != n*2) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned a = (unsigned char)s[2*i], b = (unsigned char)s[2*i+1];
        if (!((a >= '0' && a <= '9') || (a >= 'a' && a <= 'f')) ||
            !((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f'))) return false;
        out[i] = (uint8_t)(((a <= '9' ? a-'0' : a-'a'+10) << 4) |
                           (b <= '9' ? b-'0' : b-'a'+10));
    }
    return true;
}
static wiz_patch_t to_wiz(const lg_intent_t *p)
{
    return (wiz_patch_t){p->fields,p->kelvin,p->scene,p->power,p->brightness,p->red,p->green,p->blue,p->speed};
}
static lg_intent_t to_authority(const wiz_patch_t *p)
{
    return (lg_intent_t){p->fields,p->kelvin,p->scene,p->power,p->brightness,p->red,p->green,p->blue,p->speed};
}
static bool equal_patch(const wiz_patch_t *a, const wiz_patch_t *b)
{
    return a->fields == b->fields && a->kelvin == b->kelvin && a->scene == b->scene &&
        a->power == b->power && a->brightness == b->brightness && a->red == b->red &&
        a->green == b->green && a->blue == b->blue && a->speed == b->speed;
}
static bool authorize(void *ctx, uint8_t index, const uint8_t id[16], const wiz_patch_t *desired, uint64_t now)
{
    lgs_service_t *s = ctx; lg_intent_t intent;
    if (s->faulted || lg_check_send(&s->authority, id, index, now, &intent) != LG_OK) return false;
    wiz_patch_t patch = to_wiz(&intent);
    return equal_patch(&patch, desired);
}
static wiz_error_t resolve(void *ctx, const uint8_t mac[6], wiz_peer_t *peer)
{ lgs_service_t *s = ctx; return s->driver_io.resolve(s->driver_io.context, mac, peer); }
static wiz_error_t open_io(void *ctx, const wiz_peer_t *peer)
{ lgs_service_t *s = ctx; return s->driver_io.open(s->driver_io.context, peer); }
static wiz_error_t send_io(void *ctx, const wiz_peer_t *peer, const uint8_t *data,
                            size_t n, wiz_send_guard_t *guard)
{
    lgs_service_t *s = ctx;
    wiz_error_t result=s->driver_io.send(s->driver_io.context,peer,data,n,guard);
    if(guard->checked&&guard->transmitted&&s->driver.wire_method==WIZ_SET_PILOT){
        lg_intent_t ignored;
        /* The transport rechecked policy after its Wi-Fi/lease work, immediately
         * before sendto. Mark sent only after acceptance by the local socket,
         * using that same actual-send sample, not the earlier poll timestamp. */
        if(lg_prepare_send(&s->authority,s->driver.command_id,s->driver.index,
                            guard->last_now_ms,&ignored)!=LG_OK){
            s->faulted=true;s->authority.ready=false;return WIZ_CANCELLED;
        }
    }
    return result;
}
static wiz_error_t receive_io(void *ctx, uint8_t *data, size_t cap, size_t *n, uint32_t *ip, uint16_t *port)
{ lgs_service_t *s = ctx; return s->driver_io.receive(s->driver_io.context, data, cap, n, ip, port); }
static void close_io(void *ctx)
{ lgs_service_t *s = ctx; s->driver_io.close(s->driver_io.context); }

static void fault(lgs_service_t *s)
{
    s->faulted = true; s->authority.ready = false;
    wiz_completion_t completion;
    (void)wiz_driver_cancel(&s->driver, &completion);
    memset(s->routes, 0, sizeof(s->routes));
    if (s->security.close_guests) s->security.close_guests(s->security.context);
}
static bool configure_driver(lgs_service_t *s)
{
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
        if (!s->record.bulbs[i].present) continue;
        wiz_security_t security = {.signing_required = !s->record.bulbs[i].legacy_allowed};
        /* If a key is configured, even explicitly legacy-enabled bulbs receive
         * signed requests. Authentication failures never become unsigned. */
        security.key.present = s->record.has_home_key;
        memcpy(security.key.bytes, s->record.home_key, 16);
        if (wiz_driver_enroll(&s->driver, (uint8_t)i, s->record.bulbs[i].mac, &security) != WIZ_OK) return false;
    }
    return true;
}

lgs_storage_result_t lgs_service_init(lgs_service_t *s, uint64_t boot,
                                      const lgs_storage_t *storage, const wiz_io_t *io,
                                      const lgs_security_hooks_t *security)
{
    if (!s || !boot || !storage || !io || !security || !security->sha256 ||
        !io->resolve || !io->open || !io->send || !io->receive || !io->close) return LGS_STORAGE_IO;
    memset(s, 0, sizeof(*s)); s->boot_id = boot; s->storage = *storage;
    s->driver_io = *io; s->security = *security;
    wiz_io_t wrapped = {s,resolve,open_io,send_io,receive_io,close_io,authorize};
    if (wiz_driver_init(&s->driver, &wrapped) != WIZ_OK) return LGS_STORAGE_IO;
    lgs_storage_result_t result = lgs_storage_load(storage, &s->record);
    if (result == LGS_STORAGE_ABSENT) return result;
    if (result != LGS_STORAGE_OK || enrolled_count(&s->record)>LGS_MAX_ENROLLED || lg_init(&s->authority, &s->record.policy, boot,
                                            lgs_enabled_mask(&s->record)) != LG_OK || !configure_driver(s)) {
        fault(s); return result == LGS_STORAGE_OK ? LGS_STORAGE_CORRUPT : result;
    }
    s->provisioned = true;
    return LGS_STORAGE_OK;
}

lgs_storage_result_t lgs_provision_trusted_admin(lgs_service_t *s, const lgs_profile_t profiles[2])
{
    if (!s || !profiles || s->provisioned || s->faulted) return LGS_STORAGE_CONFLICT;
    lgs_record_t *record = calloc(1, sizeof(*record));
    if (!record) return LGS_STORAGE_MEMORY;
    record->generation = 1; memcpy(record->profiles, profiles, sizeof(record->profiles));
    lg_policy_factory(&record->policy, s->boot_id);
    lgs_storage_result_t result = lgs_storage_save(&s->storage, 0, record);
    if (result == LGS_STORAGE_OK) {
        s->record = *record;
        if (lg_init(&s->authority, &record->policy, s->boot_id, 0) != LG_OK) { fault(s); result = LGS_STORAGE_CORRUPT; }
        else s->provisioned = true;
    } else fault(s);
    lgs_secure_zero(record, sizeof(*record)); free(record);
    return result;
}

void lgs_service_close_route(lgs_service_t *s, const uint8_t route[16])
{
    if (!s || !route) return;
    for (unsigned i = 0; i < LGS_MAX_ROUTES; ++i)
        if (s->routes[i].used && !memcmp(s->routes[i].route, route, 16)) memset(&s->routes[i], 0, sizeof(s->routes[i]));
}
void lgs_service_dispose(lgs_service_t *s)
{
    if(!s)return;
    wiz_completion_t ignored;(void)wiz_driver_cancel(&s->driver,&ignored);
    lgs_secure_zero(s,sizeof(*s));
}
static bool bind_route(lgs_service_t *s, const uint8_t route[16], lg_identity_t identity,uint64_t now)
{
    lgs_route_t *empty = NULL;
    for (unsigned i = 0; i < LGS_MAX_ROUTES; ++i) {
        if(s->routes[i].used&&now>=s->routes[i].expires_ms)memset(&s->routes[i],0,sizeof(s->routes[i]));
        if (s->routes[i].used && !memcmp(s->routes[i].route, route, 16)) {
            s->routes[i].identity = identity;s->routes[i].expires_ms=now>UINT64_MAX-60000?UINT64_MAX:now+60000;return true;
        }
        if (!s->routes[i].used && !empty) empty = &s->routes[i];
    }
    if (!empty) return false;
    empty->used = true; empty->identity = identity; empty->expires_ms=now>UINT64_MAX-60000?UINT64_MAX:now+60000;
    memcpy(empty->route, route, 16); return true;
}
static lg_error_t identity_for(lgs_service_t *s, lgs_role_t role, const uint8_t route[16],
                               uint64_t now, lg_identity_t *out)
{
    if (role == LGS_OWNER) { *out = owner; return LG_OK; }
    if (role != LGS_GUEST) return LG_UNAUTHORIZED;
    for (unsigned i = 0; i < LGS_MAX_ROUTES; ++i)
        if (s->routes[i].used && !memcmp(s->routes[i].route, route, 16)) {
            if(now>=s->routes[i].expires_ms){memset(&s->routes[i],0,sizeof(s->routes[i]));return LG_UNAUTHORIZED;}
            lg_snapshot_t snapshot;
            lg_error_t e = lg_snapshot(&s->authority, s->routes[i].identity, 0, now, &snapshot);
            if (e == LG_OK) {*out = s->routes[i].identity;s->routes[i].expires_ms=now>UINT64_MAX-60000?UINT64_MAX:now+60000;}
            return e;
        }
    return LG_UNAUTHORIZED;
}
bool lgs_service_route_authorized(lgs_service_t *s,lgs_role_t role,const uint8_t route[16],uint64_t now)
{
    lg_identity_t identity;
    return s&&route&&s->provisioned&&!s->faulted&&identity_for(s,role,route,now,&identity)==LG_OK;
}

static bool commit(lgs_service_t *s, lgs_record_t *next, const lg_policy_stage_t *stage, uint64_t now)
{
    if (s->record.generation == UINT64_MAX) return false;
    next->generation = s->record.generation+1;
    if (stage) next->policy = stage->policy;
    if (!lgs_record_valid(next)) return false;
    if (lgs_storage_save(&s->storage, s->record.generation, next) != LGS_STORAGE_OK) { fault(s); return false; }
    if (stage && lg_policy_activate_after_commit(&s->authority, stage, now) != LG_OK) { fault(s); return false; }
    s->record = *next;
    return true;
}

static lg_error_t guest_access(lgs_service_t *s, const uint8_t route[16], const uint8_t token[32],
                               const char *name, bool enroll, uint64_t now, lg_identity_t *identity)
{
    uint8_t hash[32];
    if (!s->security.sha256(token, 32, hash)) return LG_NOT_READY;
    for (unsigned i = 0; i < LG_MAX_GUESTS; ++i) {
        if (!s->record.grants[i].id || !constant_equal(hash, s->record.grants[i].token_hash, 32)) continue;
        const lg_guest_t *grant = &s->record.policy.guests[i];
        if (grant->revoked || grant->epoch != s->record.policy.guest_epoch) return LG_REVOKED;
        if (grant->boot_id != s->boot_id) {
            /* Caller has just proved the current guest SRP credential. Renew
             * this retained, unrevoked installation in its SAME durable slot;
             * never silently enroll a revoked/unknown token or reuse an ID. */
            lg_policy_stage_t stage;
            lg_error_t e=lg_policy_stage_begin(&s->authority,owner,&stage,now);
            uint64_t renewed=0;
            if(e==LG_OK)e=lg_stage_renew_guest(&stage,grant->id,grant->epoch,now,LG_MAX_GUEST_TTL_MS,&renewed);
            if(e!=LG_OK)return e;
            lgs_record_t *next=malloc(sizeof(*next));if(!next)return LG_OVERLOADED;
            *next=s->record;next->grants[i].id=renewed;
            bool saved=commit(s,next,&stage,now);
            lgs_secure_zero(next,sizeof(*next));free(next);
            if(!saved)return LG_NOT_READY;
            *identity=(lg_identity_t){renewed,s->record.policy.guest_epoch};
            return bind_route(s,route,*identity,now)?LG_OK:LG_OVERLOADED;
        }
        if (now >= grant->expires_ms) return LG_EXPIRED;
        *identity = (lg_identity_t){grant->id, grant->epoch};
        return bind_route(s, route, *identity,now) ? LG_OK : LG_OVERLOADED;
    }
    if (!enroll) return LG_REVOKED;
    if (!lgs_name_valid(name)) return LG_BAD_ARGUMENT;
    if (now < s->next_enrollment_ms) return LG_OVERLOADED;
    s->next_enrollment_ms = now > UINT64_MAX-30000 ? UINT64_MAX : now+30000;
    lg_policy_stage_t stage;
    lg_error_t error = lg_policy_stage_begin(&s->authority, owner, &stage, now);
    uint64_t id = 0;
    if (error == LG_OK) error = lg_stage_admit_guest(&stage, stage.policy.guest_epoch, now, LG_MAX_GUEST_TTL_MS, &id);
    if (error != LG_OK) return error;
    lgs_record_t *next = malloc(sizeof(*next));
    if (!next) return LG_OVERLOADED;
    *next = s->record;
    for (unsigned i = 0; i < LG_MAX_GUESTS; ++i) {
        if (stage.policy.guests[i].id != id) continue;
        next->grants[i] = (lgs_grant_t){.id = id};
        memcpy(next->grants[i].token_hash, hash, 32); strcpy(next->grants[i].name, name);
    }
    bool saved = commit(s, next, &stage, now);
    lgs_secure_zero(next, sizeof(*next)); free(next);
    if (!saved) return LG_NOT_READY;
    *identity = (lg_identity_t){id, s->record.policy.guest_epoch};
    return bind_route(s, route, *identity,now) ? LG_OK : LG_OVERLOADED;
}

bool lgs_adopt_candidate(lgs_service_t *s, lg_identity_t identity, uint8_t index,
                         const lgs_candidate_t *candidate, const char *name, bool legacy, uint64_t now)
{
    if (!s || s->faulted || !s->provisioned || identity.session_id != LG_OWNER_SESSION ||
        identity.guest_epoch || index >= LG_MAX_LIGHTS || !candidate || !candidate->identity_verified ||
        !lgs_name_valid(name) || s->record.bulbs[index].present || enrolled_count(&s->record)>=LGS_MAX_ENROLLED ||
        (!legacy && !s->record.has_home_key) || s->driver.active || s->discovering) return false;
    wiz_peer_t peer;
    if (s->driver_io.resolve(s->driver_io.context, candidate->mac, &peer) != WIZ_OK ||
        !wiz_peer_allowed(&peer, candidate->mac)) return false;
    uint8_t enabled = (uint8_t)(s->authority.enabled | (1u << index));
    if (lg_validate_enabled_change(&s->authority, owner, enabled, now) != LG_OK) return false;
    lgs_record_t *next = malloc(sizeof(*next));
    if (!next) return false;
    *next = s->record;
    lgs_bulb_t *bulb = &next->bulbs[index];
    *bulb = (lgs_bulb_t){.present=true,.legacy_allowed=legacy};
    memcpy(bulb->mac, candidate->mac, 6); strcpy(bulb->name, name);
    bool saved = commit(s, next, NULL, now);
    lgs_secure_zero(next, sizeof(*next)); free(next);
    if (!saved) return false;
    if (lg_set_enabled_after_commit(&s->authority, owner, enabled, now) != LG_OK || !configure_driver(s)) { fault(s); return false; }
    s->driver.bulbs[index].capabilities = candidate->capabilities;
    return true;
}

static lg_ledger_entry_t *entry_for(lgs_service_t *s, const uint8_t id[16])
{
    for (unsigned i = 0; i < LG_LEDGER_COUNT; ++i)
        if (s->authority.ledger[i].result != LG_EMPTY && !memcmp(s->authority.ledger[i].command.id,id,16)) return &s->authority.ledger[i];
    return NULL;
}
void lgs_service_set_discovery(lgs_service_t *s, lgs_peer_list_fn peers, void *context)
{
    if (!s) return;
    s->list_peers=peers;s->peer_context=context;
}
static bool discovery_start(lgs_service_t *s,uint64_t now)
{
    if (!s->list_peers || s->driver.active || s->discovering || now<s->next_discovery_ms || enrolled_count(&s->record)>=LGS_MAX_ENROLLED) return false;
    lg_receipt_t receipt;
    if (lg_next(&s->authority,now,&receipt)!=LG_IDLE) return false;
    uint8_t slot=LG_MAX_LIGHTS;
    for(unsigned i=0;i<LG_MAX_LIGHTS;++i)if(!s->record.bulbs[i].present){slot=(uint8_t)i;break;}
    if(slot==LG_MAX_LIGHTS)return false;
    wiz_peer_t peers[LG_MAX_LIGHTS];size_t count=0;
    if(s->list_peers(s->peer_context,peers,LG_MAX_LIGHTS,&count)!=WIZ_OK||count>LG_MAX_LIGHTS)return false;
    memset(s->candidates,0,sizeof(s->candidates));s->candidate_count=0;
    for(size_t i=0;i<count;++i){
        if(!wiz_peer_allowed(&peers[i],peers[i].mac))continue;
        bool known=false;
        for(unsigned j=0;j<LG_MAX_LIGHTS;++j)
            if(s->record.bulbs[j].present&&!memcmp(s->record.bulbs[j].mac,peers[i].mac,6))known=true;
        for(unsigned j=0;j<s->candidate_count;++j)
            if(!memcmp(s->candidates[j].mac,peers[i].mac,6))known=true;
        if(!known)memcpy(s->candidates[s->candidate_count++].mac,peers[i].mac,6);
    }
    s->candidate_at=0;s->probe_index=slot;s->probe_phase=0;s->discovered_ms=now;
    s->next_discovery_ms=now>UINT64_MAX-15000?UINT64_MAX:now+15000;
    s->discovering=s->candidate_count!=0;return true;
}
static void discovery_cancel(lgs_service_t *s)
{
    if(!s->discovering)return;
    wiz_completion_t completion;(void)wiz_driver_cancel(&s->driver,&completion);
    (void)wiz_driver_remove(&s->driver,s->probe_index);s->discovering=false;
}
static void discovery_tick(lgs_service_t *s,uint64_t now,wiz_wall_clock_t clock)
{
    wiz_completion_t completion;
    wiz_error_t e=wiz_driver_poll(&s->driver,now,clock,&completion);
    if(s->driver.last_now_ms>now)now=s->driver.last_now_ms;
    if(e==WIZ_BUSY)return;
    lgs_candidate_t *candidate=&s->candidates[s->candidate_at];
    if(e==WIZ_OK){
        if(completion.error!=WIZ_OK)candidate->error=completion.error;
        if(completion.error==WIZ_OK){
            candidate->capabilities=s->driver.bulbs[s->probe_index].capabilities;
            candidate->identity_verified|=completion.reply.identity_present;
        }
        ++s->probe_phase;
        if(s->probe_phase>=3){
            (void)wiz_driver_remove(&s->driver,s->probe_index);
            ++s->candidate_at;s->probe_phase=0;
            if(s->candidate_at>=s->candidate_count){s->discovering=false;return;}
            candidate=&s->candidates[s->candidate_at];
        }
    }
    if(s->probe_phase==0&&!s->driver.bulbs[s->probe_index].enrolled){
        wiz_security_t security={.signing_required=s->record.has_home_key};
        security.key.present=s->record.has_home_key;memcpy(security.key.bytes,s->record.home_key,16);
        if(wiz_driver_enroll(&s->driver,s->probe_index,candidate->mac,&security)!=WIZ_OK){discovery_cancel(s);return;}
    }
    wiz_method_t method=s->probe_phase==0?WIZ_GET_SYSTEM:s->probe_phase==1?WIZ_GET_MODEL:WIZ_GET_PILOT;
    if(wiz_driver_begin(&s->driver,s->probe_index,method,NULL,NULL,now,
                        now>UINT64_MAX-2000?UINT64_MAX:now+2000)!=WIZ_OK)discovery_cancel(s);
}
void lgs_service_tick(lgs_service_t *s, uint64_t now, wiz_wall_clock_t clock)
{
    if (!s || !s->provisioned || s->faulted) return;
    s->clock_ready=clock.trusted;
    if (lg_tick(&s->authority, now) != LG_OK) { fault(s); return; }
    if(s->discovering){discovery_tick(s,now,clock);return;}
    wiz_completion_t completion;
    /* Queue reservation alone is insufficient with one physical UDP exchange:
     * preempt a stalled guest/background read when an owner command is ready.
     * A sent guest write may already have taken effect; preserve unknown status. */
    if(s->driver.active){
        lg_receipt_t next;
        if(lg_next(&s->authority,now,&next)==LG_OK){
            lg_ledger_entry_t *ready=entry_for(s,next.id);
            lg_ledger_entry_t *active=s->driver.requested_method==WIZ_SET_PILOT?entry_for(s,s->driver.command_id):NULL;
            if(ready&&ready->session_id==LG_OWNER_SESSION&&(!active||active->session_id!=LG_OWNER_SESSION)){
                if(wiz_driver_cancel(&s->driver,&completion)==WIZ_OK&&completion.requested_method==WIZ_SET_PILOT)
                    (void)lg_complete_target(&s->authority,completion.command_id,completion.index,NULL,false,now);
            }
        }
    }
    wiz_error_t polled = wiz_driver_poll(&s->driver, now, clock, &completion);
    if (s->driver.last_now_ms > now) now = s->driver.last_now_ms;
    if(s->faulted)return;
    if (polled == WIZ_BUSY) return;
    if (polled == WIZ_OK) {
        if(completion.index<LG_MAX_LIGHTS)s->bulb_errors[completion.index]=completion.error;
        if (completion.requested_method == WIZ_SET_PILOT) {
            lg_intent_t observed = to_authority(&completion.reply.state);
            (void)lg_complete_target(&s->authority, completion.command_id, completion.index,
                                     completion.observed ? &observed : NULL,
                                     completion.error == WIZ_OK && completion.observed && completion.matches, now);
        } else if (completion.requested_method == WIZ_GET_PILOT && completion.error == WIZ_OK) {
            lg_intent_t observed = to_authority(&completion.reply.state);
            (void)lg_observe(&s->authority, completion.index, s->read_revision, &observed, now);
        }
    }
    lg_receipt_t receipt;
    if (lg_next(&s->authority, now, &receipt) == LG_OK) {
        lg_ledger_entry_t *entry = entry_for(s, receipt.id);
        if (!entry) return;
        for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
            uint8_t bit = (uint8_t)(1u << i);
            if (!(entry->command.targets & bit) || ((receipt.verified | receipt.failed) & bit)) continue;
            lg_intent_t intent;
            if (lg_check_send(&s->authority, receipt.id, (uint8_t)i, now, &intent) != LG_OK) return;
            wiz_patch_t patch = to_wiz(&intent);
            if (wiz_driver_begin(&s->driver, (uint8_t)i, WIZ_SET_PILOT, &patch, receipt.id,
                                   now, entry->command.expires_ms) != WIZ_OK)
                (void)lg_complete_target(&s->authority, receipt.id, (uint8_t)i, NULL, false, now);
            return;
        }
    }
    if (now < s->next_read_ms || !s->authority.enabled) return;
    for (unsigned searched = 0; searched < LG_MAX_LIGHTS; ++searched) {
        uint8_t index = s->read_index;
        if (!(s->authority.enabled & (1u << index))) { s->read_index = (uint8_t)((index+1)%LG_MAX_LIGHTS); continue; }
        wiz_method_t method = s->read_phase == 0 ? WIZ_GET_SYSTEM : s->read_phase == 1 ? WIZ_GET_MODEL : WIZ_GET_PILOT;
        s->read_revision = s->authority.lights[index].revision;
        (void)wiz_driver_begin(&s->driver, index, method, NULL, NULL, now,
                               now > UINT64_MAX-2000 ? UINT64_MAX : now+2000);
        s->next_read_ms = now > UINT64_MAX-1000 ? UINT64_MAX : now+1000;
        if (++s->read_phase == 3) { s->read_phase = 0; s->read_index = (uint8_t)((index+1)%LG_MAX_LIGHTS); }
        return;
    }
}

static bool shape(const cJSON *j, unsigned depth)
{
    if (!j || depth > 4) return false;
    unsigned count = 0;
    for (const cJSON *a = j->child; a; a = a->next) {
        if (++count > 32 || (cJSON_IsObject(j) && !a->string)) return false;
        if (cJSON_IsObject(j)) for (const cJSON *b = a->next; b; b = b->next)
            if (b->string && !strcmp(a->string,b->string)) return false;
        if ((cJSON_IsObject(a) || cJSON_IsArray(a)) && !shape(a,depth+1)) return false;
    }
    return true;
}
static cJSON *parse_json(const uint8_t *bytes, size_t n)
{
    if (!bytes || !n || n > LGS_JSON_MAX || memchr(bytes,0,n) || !lgs_utf8_valid(bytes,n)) return NULL;
    bool quoted = false, escape = false; unsigned depth = 0, structural = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t ch = bytes[i];
        if (i+6 <= n && !memcmp(bytes+i,"\\u0000",6)) return NULL;
        if (quoted) {
            if (ch < 32) return NULL;
            if (escape) escape = false;
            else if (ch == '\\') escape = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else {
            if((ch=='{'||ch=='['||ch==','||ch==':')&&++structural>64)return NULL;
            if (ch == '{' || ch == '[') { if (++depth > 4) return NULL; }
            else if (ch == '}' || ch == ']') { if (!depth) return NULL; --depth; }
        }
    }
    if (quoted || depth) return NULL;
    const char *end = NULL;
    cJSON *j = cJSON_ParseWithLengthOpts((const char *)bytes,n,&end,false);
    if (end) while (end < (const char *)bytes+n && strchr(" \r\n\t",*end)) ++end;
    if (!cJSON_IsObject(j) || end != (const char *)bytes+n || !shape(j,0)) { cJSON_Delete(j); return NULL; }
    return j;
}
static bool keys(const cJSON *j, const char *const *allowed, size_t count)
{
    for (const cJSON *p = j->child; p; p = p->next) {
        bool match = false;
        for (size_t i = 0; i < count; ++i) if (!strcmp(p->string,allowed[i])) match = true;
        if (!match) return false;
    }
    return true;
}
static const char *string(const cJSON *j, const char *key)
{ const cJSON *p = cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsString(p) ? p->valuestring : NULL; }
static bool integer(const cJSON *j, const char *key, uint64_t max, uint64_t *out)
{
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(j,key);
    if (!cJSON_IsNumber(p) || !isfinite(p->valuedouble) || p->valuedouble < 0 ||
        floor(p->valuedouble) != p->valuedouble || p->valuedouble > (double)max) return false;
    *out = (uint64_t)p->valuedouble; return true;
}
static bool decimal(const cJSON *j, const char *key, uint64_t *out)
{
    const char *p = string(j,key);
    if (!p || !*p || strlen(p)>20 || (p[0]=='0' && p[1])) return false;
    uint64_t value = 0;
    for (; *p; ++p) {
        if (*p<'0' || *p>'9' || value>(UINT64_MAX-(uint64_t)(*p-'0'))/10) return false;
        value = value*10+(uint64_t)(*p-'0');
    }
    *out = value; return true;
}
static void add_u64(cJSON *j, const char *key, uint64_t value)
{ char text[21]; snprintf(text,sizeof(text),"%llu",(unsigned long long)value); cJSON_AddStringToObject(j,key,text); }
static void add_boot(cJSON *j, uint64_t value)
{ char text[17]; snprintf(text,sizeof(text),"%016llx",(unsigned long long)value); cJSON_AddStringToObject(j,"boot",text); }
static cJSON *response(const char *code, bool ok)
{
    cJSON *j = cJSON_CreateObject();
    if (j) { cJSON_AddNumberToObject(j,"v",1); cJSON_AddBoolToObject(j,"ok",ok); cJSON_AddStringToObject(j,"code",code); }
    return j;
}
static cJSON *error_response(lg_error_t error)
{ return response(lg_error_name(error),error == LG_OK || error == LG_DUPLICATE); }
static const char *result_name(lg_result_t result)
{
    const char *names[] = {"empty","queued","sent","verified","partial","unknown","rejected","superseded","timed_out"};
    return (unsigned)result < sizeof(names)/sizeof(names[0]) ? names[result] : "unknown";
}
static cJSON *receipt_response(lg_error_t error, const lg_receipt_t *receipt)
{
    cJSON *j = error_response(error); char id[33]; hex(receipt->id,16,id);
    if (!j) return NULL;
    cJSON_AddStringToObject(j,"id",id); cJSON_AddStringToObject(j,"result",result_name(receipt->result));
    cJSON_AddStringToObject(j,"error",lg_error_name(receipt->error));
    cJSON_AddNumberToObject(j,"sent",receipt->sent); cJSON_AddNumberToObject(j,"verified",receipt->verified);
    cJSON_AddNumberToObject(j,"failed",receipt->failed); return j;
}
static cJSON *patch_json(const lg_intent_t *p)
{
    cJSON *j = cJSON_CreateObject(); if (!j) return NULL;
    if (p->fields & LG_POWER) cJSON_AddBoolToObject(j,"power",p->power != 0);
    if (p->fields & LG_BRIGHTNESS) cJSON_AddNumberToObject(j,"brightness",p->brightness);
    if (p->fields & LG_TEMPERATURE) cJSON_AddNumberToObject(j,"kelvin",p->kelvin);
    if (p->fields & LG_SCENE) cJSON_AddNumberToObject(j,"scene",p->scene);
    if (p->fields & LG_SPEED) cJSON_AddNumberToObject(j,"speed",p->speed);
    if (p->fields & LG_RGB) { int channels[] = {p->red,p->green,p->blue}; cJSON_AddItemToObject(j,"rgb",cJSON_CreateIntArray(channels,3)); }
    return j;
}
static bool parse_patch(const cJSON *j, lg_intent_t *p)
{
    if (!cJSON_IsObject(j)) return false;
    const char *allowed[] = {"power","brightness","rgb","kelvin","scene","speed"};
    if (!keys(j,allowed,6)) return false;
    *p = (lg_intent_t){0}; uint64_t value;
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(j,"power");
    if (field) { if (!cJSON_IsBool(field)) return false; p->fields |= LG_POWER; p->power = cJSON_IsTrue(field) ? 1 : 0; }
    if (cJSON_GetObjectItemCaseSensitive(j,"brightness")) {
        if (!integer(j,"brightness",100,&value) || !value) return false;
        p->fields |= LG_BRIGHTNESS; p->brightness = (uint8_t)value;
    }
    if (cJSON_GetObjectItemCaseSensitive(j,"kelvin")) {
        if (!integer(j,"kelvin",10000,&value) || value<1000) return false;
        p->fields |= LG_TEMPERATURE; p->kelvin = (uint16_t)value;
    }
    if (cJSON_GetObjectItemCaseSensitive(j,"scene")) {
        if (!integer(j,"scene",65535,&value) || !value) return false;
        p->fields |= LG_SCENE; p->scene = (uint16_t)value;
    }
    if (cJSON_GetObjectItemCaseSensitive(j,"speed")) {
        if (!integer(j,"speed",200,&value) || value<10) return false;
        p->fields |= LG_SPEED; p->speed = (uint8_t)value;
    }
    field = cJSON_GetObjectItemCaseSensitive(j,"rgb");
    if (field) {
        if (!cJSON_IsArray(field) || cJSON_GetArraySize(field)!=3) return false;
        uint8_t values[3];
        for (int i=0;i<3;++i) {
            const cJSON *a = cJSON_GetArrayItem(field,i);
            if (!cJSON_IsNumber(a) || !isfinite(a->valuedouble) || a->valuedouble<0 ||
                a->valuedouble>255 || floor(a->valuedouble)!=a->valuedouble) return false;
            values[i] = (uint8_t)a->valuedouble;
        }
        p->fields |= LG_RGB; p->red=values[0]; p->green=values[1]; p->blue=values[2];
    }
    return p->fields != 0;
}
static cJSON *read_room(lgs_service_t *s, lg_identity_t identity, uint64_t now)
{
    lg_snapshot_t snapshot;
    lg_error_t e = lg_snapshot(&s->authority,identity,0,now,&snapshot);
    cJSON *j = error_response(e); if (!j || e!=LG_OK) return j;
    add_boot(j,snapshot.boot_id); add_u64(j,"now_ms",now); add_u64(j,"policy_epoch",snapshot.policy_epoch);
    add_u64(j,"guest_epoch",snapshot.guest_epoch); add_u64(j,"last_sequence",snapshot.last_sequence);
    cJSON_AddNumberToObject(j,"enabled",snapshot.enabled); cJSON_AddNumberToObject(j,"frozen",snapshot.frozen&snapshot.enabled);
    cJSON_AddNumberToObject(j,"max_lights",LGS_MAX_ENROLLED);
    cJSON_AddNumberToObject(j,"owner_held",snapshot.owner_held&snapshot.enabled); cJSON_AddBoolToObject(j,"storage_healthy",!s->faulted);
    unsigned slots=0;for(unsigned i=0;i<LG_MAX_GUESTS;++i)if(s->record.policy.guests[i].id)slots|=1u<<i;
    if(identity.session_id==LG_OWNER_SESSION)cJSON_AddNumberToObject(j,"guest_slots",slots);
    cJSON_AddBoolToObject(j,"driver_busy",s->driver.active);cJSON_AddBoolToObject(j,"bulb_clock_ready",s->clock_ready);return j;
}
static cJSON *read_light(lgs_service_t *s, lg_identity_t identity, uint8_t index, uint64_t now)
{
    if (index>=LG_MAX_LIGHTS || !s->record.bulbs[index].present) return error_response(LG_NOT_FOUND);
    lg_snapshot_t snapshot;
    lg_error_t e = lg_snapshot(&s->authority,identity,(uint8_t)(1u<<index),now,&snapshot);
    cJSON *j = error_response(e); if (!j || e!=LG_OK) return j;
    const lgs_bulb_t *bulb=&s->record.bulbs[index]; const lg_light_t *light=&snapshot.lights[index];
    const wiz_capabilities_t *caps=&s->driver.bulbs[index].capabilities;
    char mac[13]; hex(bulb->mac,6,mac);
    cJSON_AddNumberToObject(j,"index",index); cJSON_AddStringToObject(j,"name",bulb->name);
    cJSON_AddStringToObject(j,"error",wiz_error_name(s->bulb_errors[index]));
    cJSON_AddStringToObject(j,"mac",mac); cJSON_AddNumberToObject(j,"revision",light->revision);
    cJSON_AddBoolToObject(j,"observed_valid",light->observed_valid); add_u64(j,"observed_ms",light->observed_ms);
    cJSON_AddBoolToObject(j,"pending",light->pending); cJSON_AddStringToObject(j,"last_result",result_name(light->last_result));
    cJSON_AddItemToObject(j,"reported",patch_json(&light->observed));
    cJSON *c=cJSON_CreateObject();
    if (!c) { cJSON_Delete(j); return NULL; }
    cJSON_AddNumberToObject(c,"features",caps->features); cJSON_AddNumberToObject(c,"evidence",caps->evidence);
    cJSON_AddNumberToObject(c,"brightness_min",caps->brightness_min); cJSON_AddNumberToObject(c,"kelvin_min",caps->kelvin_min);
    cJSON_AddNumberToObject(c,"kelvin_max",caps->kelvin_max); cJSON_AddStringToObject(c,"module",caps->module);
    cJSON_AddStringToObject(c,"firmware",caps->firmware); cJSON *scenes=cJSON_CreateArray();
    if (!scenes) { cJSON_Delete(c); cJSON_Delete(j); return NULL; }
    for (unsigned i=0;i<caps->scene_count && i<WIZ_SCENE_COUNT;++i) cJSON_AddItemToArray(scenes,cJSON_CreateNumber(caps->scenes[i]));
    cJSON_AddItemToObject(c,"scenes",scenes); cJSON_AddItemToObject(j,"capabilities",c);
    cJSON_AddStringToObject(j,"reply_trust","unverified_lan");
    cJSON_AddBoolToObject(j,"legacy_control",bulb->legacy_allowed);
    cJSON_AddStringToObject(j,"permission_enforcement",
        bulb->legacy_allowed ? "gateway_only" : "requires_hardware_validation"); return j;
}

static bool command_digest(lgs_service_t *s, lg_command_t *command)
{
    /* Fixed-width canonical bytes; JSON field order/spacing never changes the
     * identity of a retry. Struct padding is deliberately not hashed. */
    uint8_t bytes[128]; size_t n=0;
#define PUT(value,count) do { uint64_t word=(value); for(unsigned k=0;k<(count);++k){bytes[n++]=(uint8_t)word;word>>=8;} } while(0)
    PUT(command->boot_id,8); PUT(command->policy_epoch,8); PUT(command->sequence,8); PUT(command->expires_ms,8);
    PUT(command->targets,1); PUT(command->owner_hold_ms,4);
    for(unsigned i=0;i<LG_MAX_LIGHTS;++i) PUT(command->expected_revision[i],4);
    const lg_intent_t *p=&command->intent;
    PUT(p->fields,2); PUT(p->kelvin,2); PUT(p->scene,2); PUT(p->power,1); PUT(p->brightness,1);
    PUT(p->red,1); PUT(p->green,1); PUT(p->blue,1); PUT(p->speed,1);
#undef PUT
    return s->security.sha256(bytes,n,command->digest);
}
static cJSON *set_command(lgs_service_t *s, lg_identity_t identity, const cJSON *j, uint64_t now, uint64_t deadline)
{
    if(s->discovering)return response("discovery_busy",false);
    const char *allowed[]={"v","op","id","boot","policy_epoch","sequence","expires_ms","targets","expected","patch","hold_ms"};
    lg_command_t c={0}; uint64_t value; uint8_t boot[8];
    if (!keys(j,allowed,11) || !unhex(string(j,"id"),c.id,16) || !unhex(string(j,"boot"),boot,8) ||
        !decimal(j,"policy_epoch",&c.policy_epoch) || !decimal(j,"sequence",&c.sequence) ||
        !decimal(j,"expires_ms",&c.expires_ms) || c.expires_ms>deadline ||
        !integer(j,"targets",255,&value) || !value ||
        !parse_patch(cJSON_GetObjectItemCaseSensitive(j,"patch"),&c.intent)) return error_response(LG_BAD_ARGUMENT);
    c.targets=(uint8_t)value;
    for(unsigned i=0;i<8;++i) c.boot_id=(c.boot_id<<8)|boot[i];
    if(cJSON_GetObjectItemCaseSensitive(j,"hold_ms")) {
        if(!integer(j,"hold_ms",60000,&value)) return error_response(LG_BAD_ARGUMENT);
        c.owner_hold_ms=(uint32_t)value;
    }
    const cJSON *revisions=cJSON_GetObjectItemCaseSensitive(j,"expected");
    if(!cJSON_IsArray(revisions)||cJSON_GetArraySize(revisions)!=8) return error_response(LG_BAD_ARGUMENT);
    for(int i=0;i<8;++i) {
        const cJSON *r=cJSON_GetArrayItem(revisions,i);
        if(!cJSON_IsNumber(r)||!isfinite(r->valuedouble)||r->valuedouble<0||r->valuedouble>UINT32_MAX||floor(r->valuedouble)!=r->valuedouble) return error_response(LG_BAD_ARGUMENT);
        c.expected_revision[i]=(uint32_t)r->valuedouble;
    }
    wiz_patch_t patch=to_wiz(&c.intent);
    for(unsigned i=0;i<LG_MAX_LIGHTS;++i) if(c.targets&(1u<<i)) {
        if(!s->record.bulbs[i].present || wiz_validate_patch(&patch,&s->driver.bulbs[i].capabilities)!=WIZ_OK)
            return response("unsupported_setting",false);
    }
    if(!command_digest(s,&c)) return error_response(LG_NOT_READY);
    lg_receipt_t receipt={0}; lg_error_t error=lg_submit(&s->authority,identity,&c,now,&receipt);
    return receipt_response(error,&receipt);
}

static cJSON *policy_change(lgs_service_t *s, const cJSON *j, const char *op, uint64_t now)
{
    lg_policy_stage_t stage; lg_error_t e=lg_policy_stage_begin(&s->authority,owner,&stage,now);
    if(e!=LG_OK) return error_response(e);
    lgs_record_t *next=malloc(sizeof(*next)); if(!next) return error_response(LG_OVERLOADED);
    *next=s->record; uint64_t value=0, duration=0; bool close=false;
    if(!strcmp(op,"freeze")) {
        const char *allowed[]={"v","op","targets","duration_ms"};
        e=keys(j,allowed,4)&&integer(j,"targets",255,&value)&&value&&!(value&~(uint64_t)s->authority.enabled)&&
          integer(j,"duration_ms",LG_MAX_GUEST_TTL_MS,&duration) ?
          lg_stage_freeze(&stage,(uint8_t)value,now,duration):LG_BAD_ARGUMENT;
    } else if(!strcmp(op,"revoke")) {
        const char *allowed[]={"v","op","grant"};
        e=LG_BAD_ARGUMENT;
        if(keys(j,allowed,3)&&decimal(j,"grant",&value)){
            bool current=false;for(unsigned i=0;i<LG_MAX_GUESTS;++i)if(s->record.grants[i].id==value)current=true;
            /* A boot renewal replaces the ID. Do not report the old visible
             * ID revoked while its newly resumed grant remains active. */
            e=current?lg_stage_revoke(&stage,value):LG_CONFLICT;
        }
        close=true;
    } else if(!strcmp(op,"revoke_all")) {
        const char *allowed[]={"v","op"}; e=keys(j,allowed,2)?lg_stage_rotate_guest_epoch(&stage):LG_BAD_ARGUMENT; close=true;
    } else if(!strcmp(op,"rotate_guest")) {
        const char *allowed[]={"v","op","salt","verifier"};
        e=keys(j,allowed,4)&&unhex(string(j,"salt"),next->profiles[1].salt,16)&&
          unhex(string(j,"verifier"),next->profiles[1].verifier,384)?lg_stage_rotate_guest_epoch(&stage):LG_BAD_ARGUMENT;
        close=true;
    } else e=LG_BAD_ARGUMENT;
    if(e==LG_OK && !commit(s,next,&stage,now)) e=LG_NOT_READY;
    lgs_secure_zero(next,sizeof(*next)); free(next);
    if(e==LG_OK && close) {
        memset(s->routes,0,sizeof(s->routes));
        if(s->security.close_guests) s->security.close_guests(s->security.context);
    }
    cJSON *reply=error_response(e); if(e==LG_OK) add_u64(reply,"policy_epoch",s->record.policy.epoch);
    return reply;
}
static cJSON *owner_config(lgs_service_t *s,const cJSON *j,const char *op,uint64_t now)
{
    if(s->driver.active) return response("driver_busy",false);
    lgs_record_t *next=malloc(sizeof(*next)); if(!next) return error_response(LG_OVERLOADED);
    *next=s->record; bool valid=false; uint8_t enabled=s->authority.enabled; uint64_t index=0;
    if(!strcmp(op,"home_key")) {
        const char *allowed[]={"v","op","key"};
        valid=keys(j,allowed,3)&&unhex(string(j,"key"),next->home_key,16);
        next->has_home_key=true;
    } else if(!strcmp(op,"remove")) {
        const char *allowed[]={"v","op","index"};
        valid=keys(j,allowed,3)&&integer(j,"index",7,&index)&&next->bulbs[index].present;
        if(valid) { next->bulbs[index]=(lgs_bulb_t){0}; enabled=(uint8_t)(enabled&~(1u<<index)); }
    }
    if(valid) valid=lg_validate_enabled_change(&s->authority,owner,enabled,now)==LG_OK;
    bool saved=valid&&commit(s,next,NULL,now);
    lgs_secure_zero(next,sizeof(*next)); free(next);
    if(!valid) return error_response(LG_BAD_ARGUMENT);
    if(!saved) return error_response(LG_NOT_READY);
    if(!strcmp(op,"remove")) (void)wiz_driver_remove(&s->driver,(uint8_t)index);
    if(lg_set_enabled_after_commit(&s->authority,owner,enabled,now)!=LG_OK||!configure_driver(s)) {fault(s);return error_response(LG_NOT_READY);}
    return error_response(LG_OK);
}
static cJSON *dispatch(lgs_service_t *s,lgs_role_t role,const uint8_t route[16],uint64_t now,uint64_t deadline,const cJSON *j)
{
    uint64_t version; const char *op=string(j,"op");
    if(!integer(j,"v",1,&version)||version!=1||!op) return error_response(LG_BAD_ARGUMENT);
    if(!s->provisioned) return response("not_provisioned",false);
    if(s->faulted) return response("storage_maintenance",false);
    if(deadline<=now) return error_response(LG_EXPIRED);
    if(!strcmp(op,"guest.enroll")||!strcmp(op,"guest.resume")) {
        if(role!=LGS_GUEST) return error_response(LG_UNAUTHORIZED);
        const char *allowed[]={"v","op","token","name"}; uint8_t token[32]; lg_identity_t identity;
        bool enroll=!strcmp(op,"guest.enroll");
        if(!keys(j,allowed,enroll?4u:3u)||!unhex(string(j,"token"),token,32)) return error_response(LG_BAD_ARGUMENT);
        lg_error_t e=guest_access(s,route,token,string(j,"name"),enroll,now,&identity); lgs_secure_zero(token,sizeof(token));
        cJSON *reply=error_response(e); if(e==LG_OK){add_u64(reply,"grant",identity.session_id);add_u64(reply,"guest_epoch",identity.guest_epoch);}return reply;
    }
    lg_identity_t identity; lg_error_t e=identity_for(s,role,route,now,&identity);
    if(e!=LG_OK) return error_response(e);
    if(!strcmp(op,"room")) {
        const char *allowed[]={"v","op"}; return keys(j,allowed,2)?read_room(s,identity,now):error_response(LG_BAD_ARGUMENT);
    }
    if(!strcmp(op,"light")) {
        const char *allowed[]={"v","op","index"}; uint64_t index;
        return keys(j,allowed,3)&&integer(j,"index",7,&index)?read_light(s,identity,(uint8_t)index,now):error_response(LG_BAD_ARGUMENT);
    }
    if(!strcmp(op,"set")) return set_command(s,identity,j,now,deadline);
    if(!strcmp(op,"command")) {
        const char *allowed[]={"v","op","id"}; uint8_t id[16]; lg_receipt_t receipt={0};
        if(!keys(j,allowed,3)||!unhex(string(j,"id"),id,16)) return error_response(LG_BAD_ARGUMENT);
        e=lg_command_status(&s->authority,identity,id,now,&receipt); return receipt_response(e,&receipt);
    }
    if(role!=LGS_OWNER) return error_response(LG_UNAUTHORIZED);
    if(!strcmp(op,"sessions")) {
        const char *allowed[]={"v","op","page"}; uint64_t page;
        if(!keys(j,allowed,3)||!integer(j,"page",15,&page)) return error_response(LG_BAD_ARGUMENT);
        cJSON *reply=error_response(LG_OK); const lg_guest_t *grant=&s->record.policy.guests[page];
        cJSON_AddNumberToObject(reply,"page",(double)page); cJSON_AddBoolToObject(reply,"present",grant->id!=0);
        if(grant->id){add_u64(reply,"grant",grant->id);cJSON_AddStringToObject(reply,"name",s->record.grants[page].name);
            add_u64(reply,"expires_ms",grant->expires_ms);cJSON_AddBoolToObject(reply,"revoked",grant->revoked||grant->epoch!=s->record.policy.guest_epoch);
            cJSON_AddBoolToObject(reply,"expired",grant->boot_id!=s->boot_id||now>=grant->expires_ms);}
        return reply;
    }
    if(!strcmp(op,"freeze")||!strcmp(op,"revoke")||!strcmp(op,"revoke_all")||!strcmp(op,"rotate_guest")) return policy_change(s,j,op,now);
    if(!strcmp(op,"home_key")||!strcmp(op,"remove")) return owner_config(s,j,op,now);
    if(!strcmp(op,"discover")) {
        const char *allowed[]={"v","op"};
        if(!keys(j,allowed,2))return error_response(LG_BAD_ARGUMENT);
        if(!s->list_peers)return response("discovery_unavailable",false);
        bool started=discovery_start(s,now);
        return response(started?"discovery_started":"discovery_busy",started);
    }
    if(!strcmp(op,"discovery_cancel")) {
        const char *allowed[]={"v","op"};
        if(!keys(j,allowed,2))return error_response(LG_BAD_ARGUMENT);
        discovery_cancel(s);return error_response(LG_OK);
    }
    if(!strcmp(op,"candidate")) {
        const char *allowed[]={"v","op","index"};uint64_t index;
        if(!keys(j,allowed,3)||!integer(j,"index",7,&index))return error_response(LG_BAD_ARGUMENT);
        cJSON *reply=error_response(LG_OK);cJSON_AddBoolToObject(reply,"discovering",s->discovering);
        cJSON_AddNumberToObject(reply,"count",s->candidate_count);cJSON_AddBoolToObject(reply,"present",index<s->candidate_count);
        if(index<s->candidate_count){const lgs_candidate_t *candidate=&s->candidates[index];char mac[13];hex(candidate->mac,6,mac);
            cJSON_AddStringToObject(reply,"mac",mac);cJSON_AddBoolToObject(reply,"identified",candidate->identity_verified);
            cJSON_AddStringToObject(reply,"error",wiz_error_name(candidate->error));
            cJSON_AddStringToObject(reply,"module",candidate->capabilities.module);cJSON_AddStringToObject(reply,"firmware",candidate->capabilities.firmware);
            cJSON_AddStringToObject(reply,"reply_trust","unverified_lan");}
        return reply;
    }
    if(!strcmp(op,"adopt")) {
        const char *allowed[]={"v","op","candidate","name","legacy"};uint64_t index;
        const cJSON *legacy=cJSON_GetObjectItemCaseSensitive(j,"legacy");
        if(!keys(j,allowed,5)||!integer(j,"candidate",7,&index)||index>=s->candidate_count||
            !cJSON_IsBool(legacy)||!lgs_name_valid(string(j,"name")))return error_response(LG_BAD_ARGUMENT);
        if(s->discovering||now<s->discovered_ms||now-s->discovered_ms>=300000)return response("discovery_required",false);
        uint8_t slot=LG_MAX_LIGHTS;for(unsigned i=0;i<LG_MAX_LIGHTS;++i)if(!s->record.bulbs[i].present){slot=(uint8_t)i;break;}
        bool adopted=slot<LG_MAX_LIGHTS&&lgs_adopt_candidate(s,owner,slot,&s->candidates[index],string(j,"name"),cJSON_IsTrue(legacy),now);
        cJSON *reply=response(adopted?"ok":"adoption_rejected",adopted);if(adopted)cJSON_AddNumberToObject(reply,"index",slot);return reply;
    }
    return response("unsupported_operation",false);
}
static void wipe_json(cJSON *j)
{
    if(!j)return;
    if(j->valuestring) lgs_secure_zero(j->valuestring,strlen(j->valuestring));
    for(cJSON *p=j->child;p;p=p->next)wipe_json(p);
}
bool lgs_service_request(lgs_service_t *s,lgs_role_t role,const uint8_t route[16],uint64_t now,
                          uint64_t deadline,const uint8_t *json,size_t n,uint8_t **reply,size_t *reply_size)
{
    if(!s||!route||!reply||!reply_size)return false;
    *reply=NULL;*reply_size=0;
    cJSON *request=parse_json(json,n);
    cJSON *result=request?dispatch(s,role,route,now,deadline,request):error_response(LG_BAD_ARGUMENT);
    wipe_json(request);cJSON_Delete(request);
    if(!result)return false;
    char *encoded=cJSON_PrintUnformatted(result);cJSON_Delete(result);
    if(!encoded)return false;
    size_t size=strlen(encoded);
    if(!size||size>LGS_JSON_MAX){lgs_secure_zero(encoded,size);free(encoded);return false;}
    *reply=(uint8_t *)encoded;*reply_size=size;return true;
}
