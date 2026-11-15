#ifndef LIGHTING_SESSION_IDF_H
#define LIGHTING_SESSION_IDF_H

#include "lighting_session_wire.h"
#include "esp_err.h"
#include "protocomm_security.h"
#include "psa/crypto.h"

#define LS_SESSION_COUNT 3u
#define LS_GUEST_CONNECTIONS 2u
#define LS_HANDSHAKE_MS 10000u
#define LS_IDLE_MS 60000u
#define LS_REQUEST_LIMIT 65536u

typedef struct {
    uint8_t route[16];
    uint32_t crypto_id;
    protocomm_security_handle_t handle;
    psa_hash_operation_t transcript;
    uint8_t transcript_digest[32];
    uint64_t expires_at;
    uint32_t requests;
    unsigned phase;
    ls_role_t credential;
    bool bound;
} ls_session_t;

/* All calls are serialized by ONE idle-priority application worker. Never call
 * from a BLE host callback. Each entry owns its own Security2 instance. */
typedef struct {
    ls_session_t sessions[LS_SESSION_COUNT];
    protocomm_security2_params_t owner, guest;
    uint8_t boot[8];
    char gateway[49];
    uint32_t next_crypto_id;
    uint64_t next_handshake_at;
} ls_manager_t;

typedef bool (*ls_sign_fn)(const char *context, char signature_hex[161], void *context_arg);
typedef esp_err_t (*ls_application_fn)(ls_role_t credential, const uint8_t route[16],
                                      const uint8_t *request, size_t request_size,
                                      uint8_t **reply, size_t *reply_size, void *context_arg);

/* Salt/verifier storage must remain alive until dispose. No plaintext passwords
 * are stored here. Exactly16-byte salt and384-byte RFC5054 verifier required. */
esp_err_t ls_manager_init(ls_manager_t *manager, const char *gateway, const uint8_t boot[8],
                          const protocomm_security2_params_t *owner,
                          const protocomm_security2_params_t *guest);
void ls_manager_dispose(ls_manager_t *manager);
void ls_manager_expire(ls_manager_t *manager, uint64_t now_ms);
void ls_manager_close(ls_manager_t *manager, const uint8_t route[16]);
void ls_manager_close_guests(ls_manager_t *manager);

/* Returned buffers are caller-owned. On any crypto/protocol error the session
 * is destroyed; the client must make a fresh handshake on either transport. */
esp_err_t ls_manager_handshake(ls_manager_t *manager, const uint8_t route[16],
                               const uint8_t *request, size_t size, uint64_t now_ms,
                               uint8_t **reply, size_t *reply_size);
esp_err_t ls_manager_data(ls_manager_t *manager, const uint8_t route[16],
                          const uint8_t *request, size_t size, uint64_t now_ms,
                          ls_sign_fn sign, ls_application_fn application, void *context_arg,
                          uint8_t **reply, size_t *reply_size);
ls_role_t ls_manager_bound_role(const ls_manager_t *manager, const uint8_t route[16]);
/* Admission priority only: client SRP proof checked, identity binding/grant
 * authorization still required before application controls. */
ls_role_t ls_manager_proven_role(const ls_manager_t *manager, const uint8_t route[16]);

#endif
