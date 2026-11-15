#include "lighting_session_idf.h"
#include "protocomm_security2.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static ls_session_t *find(ls_manager_t *m, const uint8_t route[16])
{
    for (unsigned i = 0; i < LS_SESSION_COUNT; ++i)
        if (m->sessions[i].handle && !memcmp(m->sessions[i].route, route, 16))
            return &m->sessions[i];
    return NULL;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static void close_session(ls_session_t *s)
{
    psa_hash_abort(&s->transcript);
    if (s->handle) protocomm_security2.cleanup(s->handle);
    wipe(s, sizeof(*s));
}

static bool profile_valid(const protocomm_security2_params_t *profile)
{
    return profile && profile->salt && profile->salt_len == 16 &&
           profile->verifier && profile->verifier_len == 384;
}

esp_err_t ls_manager_init(ls_manager_t *m, const char *gateway, const uint8_t boot[8],
                          const protocomm_security2_params_t *owner,
                          const protocomm_security2_params_t *guest)
{
    uint8_t zero[32] = {0}; char context[LS_BINDING_CONTEXT_MAX];
    if (!m || !profile_valid(owner) || !profile_valid(guest) ||
        !ls_binding_context(context, gateway, boot, zero, zero, LS_ROLE_OWNER))
        return ESP_ERR_INVALID_ARG;
    memset(m, 0, sizeof(*m));
    strcpy(m->gateway, gateway); memcpy(m->boot, boot, 8);
    m->owner = *owner; m->guest = *guest;
    /* Build also caps these components at INFO. Runtime lowering alone cannot
     * guarantee that a later wildcard DEBUG setting will not expose SDK keys. */
    esp_log_level_set("security2", ESP_LOG_ERROR);
    esp_log_level_set("srp6a", ESP_LOG_ERROR);
    return ESP_OK;
}

void ls_manager_dispose(ls_manager_t *m)
{
    if (!m) return;
    for (unsigned i = 0; i < LS_SESSION_COUNT; ++i) close_session(&m->sessions[i]);
    wipe(m, sizeof(*m));
}

void ls_manager_expire(ls_manager_t *m, uint64_t now)
{
    if (!m) return;
    for (unsigned i = 0; i < LS_SESSION_COUNT; ++i)
        if (m->sessions[i].handle && now >= m->sessions[i].expires_at)
            close_session(&m->sessions[i]);
}

void ls_manager_close(ls_manager_t *m, const uint8_t route[16])
{
    if (!m || !route) return;
    ls_session_t *s = find(m, route);
    if (s) close_session(s);
}

void ls_manager_close_guests(ls_manager_t *m)
{
    if (!m) return;
    for (unsigned i = 0; i < LS_SESSION_COUNT; ++i)
        if (m->sessions[i].credential == LS_ROLE_GUEST) close_session(&m->sessions[i]);
}

static bool hash_record(ls_session_t *s, const uint8_t *data, size_t n)
{
    const uint8_t prefix[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16),
                               (uint8_t)(n >> 8), (uint8_t)n};
    return psa_hash_update(&s->transcript, prefix, sizeof(prefix)) == PSA_SUCCESS &&
           psa_hash_update(&s->transcript, data, n) == PSA_SUCCESS;
}

esp_err_t ls_manager_handshake(ls_manager_t *m, const uint8_t route[16],
                               const uint8_t *request, size_t n, uint64_t now,
                               uint8_t **reply, size_t *reply_size)
{
    if (!m || !route || !reply || !reply_size) return ESP_ERR_INVALID_ARG;
    *reply = NULL; *reply_size = 0;
    ls_manager_expire(m, now);
    ls_session_t *s = find(m, route);
    ls_role_t credential = s ? s->credential : LS_ROLE_NONE;
    unsigned step = s ? s->phase : 0;
    if (step > 1 || !ls_client_handshake(request, n, step, &credential)) {
        if (s) close_session(s);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s) {
        if (now < m->next_handshake_at || m->next_crypto_id == UINT32_MAX)
            return ESP_ERR_INVALID_STATE;
        unsigned guests = 0;
        for (unsigned i = 0; i < LS_SESSION_COUNT; ++i) {
            ls_session_t *other = &m->sessions[i];
            if (other->handle && other->phase < 2) return ESP_ERR_INVALID_STATE;
            if (other->handle && other->credential == LS_ROLE_GUEST) ++guests;
            if (other->handle && other->credential == LS_ROLE_OWNER &&
                credential == LS_ROLE_OWNER) return ESP_ERR_INVALID_STATE;
            if (!other->handle && !s) s = other;
        }
        if (!s || (credential == LS_ROLE_GUEST && guests >= LS_GUEST_CONNECTIONS))
            return ESP_ERR_NO_MEM;
        m->next_handshake_at = now > UINT64_MAX - 1000 ? UINT64_MAX : now + 1000;
        if (protocomm_security2.init(&s->handle) != ESP_OK) return ESP_ERR_NO_MEM;
        s->crypto_id = ++m->next_crypto_id; s->credential = credential;
        memcpy(s->route, route, 16);
        if (protocomm_security2.new_transport_session(s->handle, s->crypto_id) != ESP_OK ||
            psa_hash_setup(&s->transcript, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            close_session(s); return ESP_FAIL;
        }
        s->expires_at = now > UINT64_MAX - LS_HANDSHAKE_MS ? UINT64_MAX : now + LS_HANDSHAKE_MS;
    }
    ssize_t out_size = 0;
    const protocomm_security2_params_t *profile = credential == LS_ROLE_OWNER ? &m->owner : &m->guest;
    esp_err_t err = protocomm_security2.security_req_handler(s->handle, profile, s->crypto_id,
                                                             request, (ssize_t)n, reply, &out_size, NULL);
    if (err != ESP_OK || out_size <= 0 || out_size > LS_PACKET_MAX || !*reply ||
        !hash_record(s, request, n) || !hash_record(s, *reply, (size_t)out_size)) {
        free(*reply); *reply = NULL; close_session(s);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    if (++s->phase == 2) {
        size_t digest_size = 0;
        if (psa_hash_finish(&s->transcript, s->transcript_digest, 32, &digest_size) != PSA_SUCCESS ||
            digest_size != 32) {
            free(*reply); *reply = NULL; close_session(s); return ESP_FAIL;
        }
        /* The binding request still has the original admission deadline. */
    }
    *reply_size = (size_t)out_size;
    return ESP_OK;
}

static void hex(const uint8_t *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2*i] = digits[bytes[i] >> 4]; out[2*i+1] = digits[bytes[i] & 15];
    }
    out[2*n] = 0;
}

static esp_err_t bind_reply(ls_manager_t *m, ls_session_t *s, const uint8_t *plain, size_t n,
                            ls_sign_fn sign, void *arg, uint8_t **out, size_t *size)
{
    /* bind is deliberately the only plaintext operation allowed before device
     * identity is proven. It accepts a challenge, never a digest to be signed. */
    if (!sign || n > 160 || memchr(plain, 0, n) || memchr(plain, '\\', n)) return ESP_ERR_INVALID_ARG;
    const char *end = NULL;
    cJSON *j = cJSON_ParseWithLengthOpts((const char *)plain, n, &end, false);
    if (!j || !cJSON_IsObject(j) || end != (const char *)plain + n) {
        cJSON_Delete(j); return ESP_ERR_INVALID_ARG;
    }
    unsigned seen = 0;
    uint8_t challenge[32];
    bool valid = true;
    for (const cJSON *it = j->child; it; it = it->next) {
        unsigned bit = !strcmp(it->string, "v") ? 1 : !strcmp(it->string, "op") ? 2 :
                       !strcmp(it->string, "challenge") ? 4 : 0;
        if (!bit || (seen & bit)) { valid = false; break; }
        seen |= bit;
        if (bit == 1 && (!cJSON_IsNumber(it) || it->valuedouble != 1)) valid = false;
        if (bit == 2 && (!cJSON_IsString(it) || strcmp(it->valuestring, "bind"))) valid = false;
        if (bit == 4 && (!cJSON_IsString(it) ||
                         !ls_lower_hex(it->valuestring, strlen(it->valuestring), challenge, 32))) valid = false;
    }
    cJSON_Delete(j);
    if (!valid || seen != 7) return ESP_ERR_INVALID_ARG;
    char context[LS_BINDING_CONTEXT_MAX], signature[161];
    if (!ls_binding_context(context, m->gateway, m->boot, s->transcript_digest, challenge,
                             s->credential) || !sign(context, signature, arg)) return ESP_FAIL;
    size_t siglen = strnlen(signature, sizeof(signature)); uint8_t sigbytes[80];
    if (siglen < 16 || siglen > 144 || siglen % 2 ||
        !ls_lower_hex(signature, siglen, sigbytes, siglen / 2)) return ESP_FAIL;
    char boot[17], transcript[65], nonce[65];
    hex(m->boot, 8, boot); hex(s->transcript_digest, 32, transcript); hex(challenge, 32, nonce);
    j = cJSON_CreateObject();
    if (!j || !cJSON_AddNumberToObject(j, "v", 1) || !cJSON_AddStringToObject(j, "op", "bound") ||
        !cJSON_AddStringToObject(j, "gateway_id", m->gateway) ||
        !cJSON_AddStringToObject(j, "boot", boot) || !cJSON_AddStringToObject(j, "transcript", transcript) ||
        !cJSON_AddStringToObject(j, "challenge", nonce) ||
        !cJSON_AddStringToObject(j, "role", s->credential == LS_ROLE_OWNER ? "owner" : "guest") ||
        !cJSON_AddStringToObject(j, "signature", signature)) { cJSON_Delete(j); return ESP_ERR_NO_MEM; }
    char *json = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    if (!json) return ESP_ERR_NO_MEM;
    *out = (uint8_t *)json; *size = strlen(json);
    return ESP_OK;
}

esp_err_t ls_manager_data(ls_manager_t *m, const uint8_t route[16], const uint8_t *request,
                          size_t n, uint64_t now, ls_sign_fn sign, ls_application_fn app,
                          void *arg, uint8_t **reply, size_t *reply_size)
{
    if (!m || !route || !reply || !reply_size) return ESP_ERR_INVALID_ARG;
    *reply = NULL; *reply_size = 0;
    ls_manager_expire(m, now);
    ls_session_t *s = find(m, route);
    if (!s) return ESP_ERR_NOT_FOUND;
    if (!request || n <= 16 || n > LS_PACKET_MAX || s->phase != 2 ||
        s->requests >= LS_REQUEST_LIMIT) { close_session(s); return ESP_ERR_INVALID_STATE; }
    uint8_t *plain = NULL, *answer = NULL;
    ssize_t plain_size = 0, encrypted_size = 0;
    size_t answer_size = 0;
    esp_err_t err = protocomm_security2.decrypt(s->handle, s->crypto_id, request, (ssize_t)n,
                                               &plain, &plain_size);
    if (err == ESP_OK && plain_size > 0 && plain_size <= LS_PLAINTEXT_MAX) {
        if (!s->bound) {
            err = bind_reply(m, s, plain, (size_t)plain_size, sign, arg, &answer, &answer_size);
            if (err == ESP_OK) s->bound = true;
        } else if (app) {
            err = app(s->credential, s->route, plain, (size_t)plain_size, &answer, &answer_size, arg);
        } else err = ESP_ERR_INVALID_STATE;
    } else if (err == ESP_OK) err = ESP_ERR_INVALID_SIZE;
    if (plain) { wipe(plain, plain_size > 0 ? (size_t)plain_size : 0); free(plain); }
    if (err == ESP_OK && (!answer || !answer_size || answer_size > LS_PLAINTEXT_MAX)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK)
        err = protocomm_security2.encrypt(s->handle, s->crypto_id, answer, (ssize_t)answer_size,
                                           reply, &encrypted_size);
    if (answer) { wipe(answer, answer_size); free(answer); }
    if (err != ESP_OK || encrypted_size <= 16 || encrypted_size > LS_PACKET_MAX) {
        free(*reply); *reply = NULL; close_session(s);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    ++s->requests;
    s->expires_at = now > UINT64_MAX - LS_IDLE_MS ? UINT64_MAX : now + LS_IDLE_MS;
    *reply_size = (size_t)encrypted_size;
    return ESP_OK;
}

ls_role_t ls_manager_bound_role(const ls_manager_t *m, const uint8_t route[16])
{
    if (!m || !route) return LS_ROLE_NONE;
    for (unsigned i = 0; i < LS_SESSION_COUNT; ++i)
        if (m->sessions[i].handle && m->sessions[i].bound &&
            !memcmp(m->sessions[i].route, route, 16)) return m->sessions[i].credential;
    return LS_ROLE_NONE;
}

ls_role_t ls_manager_proven_role(const ls_manager_t *m, const uint8_t route[16])
{
    if (!m || !route) return LS_ROLE_NONE;
    for (unsigned i = 0; i < LS_SESSION_COUNT; ++i)
        if (m->sessions[i].handle && m->sessions[i].phase == 2 &&
            !memcmp(m->sessions[i].route, route, 16)) return m->sessions[i].credential;
    return LS_ROLE_NONE;
}
