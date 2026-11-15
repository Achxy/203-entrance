#ifndef LIGHTS_GATEWAY_SERVICE_H
#define LIGHTS_GATEWAY_SERVICE_H
#include "gateway_storage.h"
#include "wiz_driver.h"

#define LGS_MAX_ROUTES 3u
#define LGS_MAX_ENROLLED 5u
#define LGS_JSON_MAX 1008u
typedef enum { LGS_GUEST = 1, LGS_OWNER = 2 } lgs_role_t;
typedef struct { uint8_t route[16]; lg_identity_t identity; uint64_t expires_ms; bool used; } lgs_route_t;
/* identity_verified means matched MAC plus associated-AP peer identity, not a
 * cryptographic signature on the WiZ response. Enforcement remains unproven. */
typedef struct { uint8_t mac[6]; wiz_capabilities_t capabilities; wiz_error_t error; bool identity_verified; } lgs_candidate_t;
typedef wiz_error_t (*lgs_peer_list_fn)(void *context, wiz_peer_t *peers,
                                       size_t capacity, size_t *count);
typedef struct {
    bool (*sha256)(const void *, size_t, uint8_t out[32]);
    void (*close_guests)(void *context);
    void *context;
} lgs_security_hooks_t;

typedef struct {
    lgs_record_t record;
    lg_core_t authority;
    wiz_driver_t driver;
    wiz_io_t driver_io;
    lgs_storage_t storage;
    lgs_security_hooks_t security;
    lgs_route_t routes[LGS_MAX_ROUTES];
    uint64_t boot_id, next_enrollment_ms, next_read_ms;
    uint32_t read_revision;
    wiz_error_t bulb_errors[LG_MAX_LIGHTS];
    uint8_t read_index, read_phase;
    bool provisioned, faulted, clock_ready;
    lgs_peer_list_fn list_peers;
    void *peer_context;
    lgs_candidate_t candidates[LG_MAX_LIGHTS];
    uint64_t discovered_ms, next_discovery_ms;
    uint8_t candidate_count, candidate_at, probe_index, probe_phase;
    bool discovering;
} lgs_service_t;

/* No allocation or mutation of the production app occurs until its integration
 * calls this initializer. All functions run on one serialized low-priority
 * worker, including persistence, driver polling and application requests. */
lgs_storage_result_t lgs_service_init(lgs_service_t *service, uint64_t boot_id,
                                      const lgs_storage_t *storage, const wiz_io_t *io,
                                      const lgs_security_hooks_t *security);
/* ONLY existing authenticated management/USB owner provisioning may call this.
 * It is not exposed to an unclaimed public or guest endpoint. */
lgs_storage_result_t lgs_provision_trusted_admin(lgs_service_t *service,
                                                const lgs_profile_t profiles[2]);
void lgs_service_close_route(lgs_service_t *service, const uint8_t route[16]);
void lgs_service_dispose(lgs_service_t *service);
bool lgs_service_route_authorized(lgs_service_t *service, lgs_role_t proven_role,
                                   const uint8_t route[16], uint64_t now_ms);
void lgs_service_tick(lgs_service_t *service, uint64_t now_ms, wiz_wall_clock_t clock);
void lgs_service_set_discovery(lgs_service_t *service, lgs_peer_list_fn peers, void *context);
/* The caller supplies a proven role and ORIGINAL transport deadline. A caller
 * cannot grant authority using a JSON role field or reset a stale deadline. */
bool lgs_service_request(lgs_service_t *service, lgs_role_t proven_role,
                          const uint8_t route[16], uint64_t now_ms,
                          uint64_t transport_deadline_ms,
                          const uint8_t *json, size_t json_size,
                          uint8_t **reply, size_t *reply_size);

/* Trusted AP discovery path reports a read-only identity/capability probe here.
 * No remote client can supply an address or manufacture discovery evidence. */
bool lgs_adopt_candidate(lgs_service_t *service, lg_identity_t owner, uint8_t index,
                         const lgs_candidate_t *candidate, const char *name,
                         bool legacy_opt_in, uint64_t now_ms);

#endif
