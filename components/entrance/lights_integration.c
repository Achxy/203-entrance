#include "lights_integration.h"
#include "arcade.h"
#include "relay.h"
#include <string.h>

#if ENTRANCE_LIGHTS
#include "lighting_runtime.h"
#include "esp_tls.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "mbedtls/base64.h"
#include <math.h>

typedef struct {
    uint8_t route[16], payload[1024];
    uint32_t id;
    uint16_t status, size;
    lrt_reply_role_t role;
} light_reply_t;
static StaticSemaphore_t peak_storage;
static SemaphoreHandle_t peak_mutex;
static StaticSemaphore_t pause_storage;
static SemaphoreHandle_t pause_mutex;
static unsigned pause_users;
static _Atomic(QueueHandle_t) replies;

bool lights_peak_begin(void)
{
    return peak_mutex && xSemaphoreTakeRecursive(peak_mutex, 0) == pdTRUE;
}
void lights_peak_end(void)
{
    if (peak_mutex) xSemaphoreGiveRecursive(peak_mutex);
}
static bool begin_peak(lrt_peak_t peak, void *ctx)
{
    (void)peak; (void)ctx;
    return lights_peak_begin();
}
static void end_peak(lrt_peak_t peak, void *ctx)
{
    (void)peak; (void)ctx;
    lights_peak_end();
}
static esp_netif_t *ap_netif(void *ctx)
{
    (void)ctx;
    return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
}
static bool sign_identity(const char *message, char signature[161], void *ctx)
{
    (void)ctx;
    return arcade_device_sign(message, signature);
}
static bool quiescing(void *ctx)
{
    (void)ctx;
    return ota_busy() || release_checking() || !settings_healthy;
}
static bool wall_clock(llt_clock_t *sample, void *ctx)
{
    (void)ctx;
    return net_trusted_clock(sample);
}
static void relay_reply(const uint8_t route[16], uint32_t id, uint16_t status,
                        const uint8_t *payload, size_t size, lrt_reply_role_t role, void *ctx)
{
    (void)ctx;
    if (!replies || size > 1024 || (size && !payload)) return;
    light_reply_t item = {.id = id, .status = status, .size = (uint16_t)size, .role = role};
    memcpy(item.route, route, 16);
    if (size) memcpy(item.payload, payload, size);
    if (xQueueSend(replies, &item, 0) != pdTRUE)
        lighting_runtime_close_relay(route);
    memset(&item, 0, sizeof(item));
}
static bool ensure_replies(void)
{
    if (replies) return true;
    if (!lights_peak_begin()) return false;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 48u * 1024u &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 24u * 1024u)
        replies = xQueueCreate(3, sizeof(light_reply_t));
    lights_peak_end();
    return replies != NULL;
}
esp_err_t lights_prepare(void)
{
    peak_mutex = xSemaphoreCreateRecursiveMutexStatic(&peak_storage);
    pause_mutex = xSemaphoreCreateMutexStatic(&pause_storage);
    if (!peak_mutex || !pause_mutex) return ESP_ERR_NO_MEM;
    lrt_config_t cfg = {.ap_netif = ap_netif, .sign_identity = sign_identity,
        .quiescing = quiescing, .begin_peak = begin_peak, .end_peak = end_peak,
        .wall_clock = wall_clock, .relay_reply = relay_reply};
    strlcpy(cfg.gateway_id, identity.device_id, sizeof(cfg.gateway_id));
    if (!hex_decode(arcade_boot_id(), cfg.boot, sizeof(cfg.boot))) return ESP_ERR_INVALID_STATE;
    return lighting_runtime_prepare(&cfg);
}
esp_err_t lights_start(void)
{
    return lighting_runtime_start();
}
void lights_poll_recovery(uint64_t now_ms)
{
    lighting_runtime_poll_recovery(now_ms);
}
bool lights_quiesce(void)
{
    if (!pause_mutex) return true;
    xSemaphoreTake(pause_mutex, portMAX_DELAY);
    bool okay = lighting_runtime_quiesce(7000);
    if (okay) ++pause_users;
    else if (!pause_users) lighting_runtime_resume();
    xSemaphoreGive(pause_mutex);
    return okay;
}
void lights_resume(void)
{
    if (!pause_mutex) return;
    xSemaphoreTake(pause_mutex, portMAX_DELAY);
    if (pause_users && !--pause_users) lighting_runtime_resume();
    xSemaphoreGive(pause_mutex);
}
bool lights_relay_ready(void)
{
    lrt_status_t s;
    lighting_runtime_status(&s);
    return s.active && s.storage_healthy && !s.quiesced && !s.resource_blocked && ensure_replies();
}
static bool integer(const cJSON *j, const char *key, uint64_t min, uint64_t max, uint64_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || floor(v->valuedouble) != v->valuedouble ||
        v->valuedouble < (double)min || v->valuedouble > (double)max) return false;
    *out = (uint64_t)v->valuedouble;
    return true;
}
static const char *field(const cJSON *j, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool lower_hex(const char *text, uint8_t *out, size_t size)
{
    if (!text || strlen(text) != size * 2) return false;
    for (size_t i = 0; i < size * 2; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return false;
    return hex_decode(text, out, size);
}
bool lights_relay_message(const cJSON *j, bool *handled)
{
    const char *type = field(j, "type");
    *handled = type && (!strcmp(type, "light_request") || !strcmp(type, "light_disconnect"));
    if (!*handled) return true;
    const char *connection = field(j, "connection"), *boot = field(j, "boot");
    lrq_request_t request = {0};
    if (!lower_hex(connection, request.route, 16) || !lower_hex(boot, request.boot, 8) ||
        strcmp(boot, arcade_boot_id())) return false;
    if (!strcmp(type, "light_disconnect")) {
        if (cJSON_GetArraySize(j) != 4) return false;
        lighting_runtime_close_relay(request.route);
        return true;
    }
    const char *op = field(j, "op"), *encoded = field(j, "payload"), *hint = field(j, "role_hint");
    uint64_t id, lease, ttl;
    if (!op || !encoded || cJSON_GetArraySize(j) != (hint ? 10 : 9) ||
        (hint && strcmp(hint, "owner") && strcmp(hint, "guest") && strcmp(hint, "unknown")) ||
        !integer(j, "id", 1, UINT32_MAX, &id) ||
        !integer(j, "lease", 0, 9007199254740991ULL, &lease) ||
        !integer(j, "ttl_ms", 1, 10000, &ttl)) return false;
    if (!strcmp(op, "identity")) request.operation = 0;
    else if (!strcmp(op, "session")) request.operation = 1;
    else if (!strcmp(op, "data")) request.operation = 2;
    else if (!strcmp(op, "close")) request.operation = 3;
    else return false;
    if ((request.operation >= 2 && ttl > 2000) || strlen(encoded) > 1368) return false;
    uint8_t decoded[1024];
    unsigned char canonical[1369];
    size_t used = 0, canonical_size = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded), &used, (const unsigned char *)encoded,
                             strlen(encoded)) != 0 ||
        mbedtls_base64_encode(canonical, sizeof(canonical), &canonical_size, decoded, used) != 0 ||
        strlen(encoded) != canonical_size || memcmp(encoded, canonical, canonical_size) ||
        (request.operation == 3 ? used != 0 : used == 0)) return false;
    request.id = (uint32_t)id; request.lease_ms = lease; request.ttl_ms = (uint32_t)ttl;
    request.payload = decoded; request.size = used;
    lrq_result_t result = lighting_runtime_submit_relay(&request);
    if (result != LRQ_OK) {
        lighting_runtime_close_relay(request.route);
        relay_reply(request.route, request.id, result == LRQ_RATE ? 429 : 503,
                    NULL, 0, LRT_ROLE_CLOSED, NULL);
    }
    memset(decoded, 0, sizeof(decoded));
    return true;
}
cJSON *lights_relay_next_reply(void)
{
    light_reply_t item;
    if (!replies || xQueueReceive(replies, &item, 0) != pdTRUE) return NULL;
    char route[33];
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        route[i*2] = digits[item.route[i] >> 4]; route[i*2+1] = digits[item.route[i] & 15];
    }
    route[32] = 0;
    unsigned char encoded[1369]; size_t size = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &size, item.payload, item.size) != 0)
        return NULL;
    encoded[size] = 0;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "v", 1); cJSON_AddStringToObject(j, "type", "light_reply");
    cJSON_AddStringToObject(j, "connection", route); cJSON_AddStringToObject(j, "boot", arcade_boot_id());
    cJSON_AddNumberToObject(j, "id", item.id); cJSON_AddNumberToObject(j, "status", item.status);
    cJSON_AddStringToObject(j, "payload", (const char *)encoded);
    if (item.role != LRT_ROLE_OMIT)
        cJSON_AddStringToObject(j, "role", item.role == LRT_ROLE_OWNER ? "owner" :
                                item.role == LRT_ROLE_GUEST ? "guest" : "none");
    memset(&item, 0, sizeof(item));
    return j;
}
void lights_relay_disconnected(void)
{
    lighting_runtime_relay_disconnected();
    if (replies) xQueueReset(replies);
}
cJSON *lights_status(void)
{
    lrt_status_t s; lighting_runtime_status(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "compiled", true);
    cJSON_AddBoolToObject(j, "configured", s.configuration_present);
    cJSON_AddBoolToObject(j, "provisioning", s.provisioning);
    cJSON_AddBoolToObject(j, "active", s.active);
    cJSON_AddBoolToObject(j, "bluetooth", s.ble_running);
    cJSON_AddBoolToObject(j, "paused", s.quiesced);
    cJSON_AddBoolToObject(j, "storage_healthy", s.storage_healthy);
    cJSON_AddBoolToObject(j, "resource_blocked", s.resource_blocked);
    cJSON_AddBoolToObject(j, "worker_running", s.worker_running);
    cJSON_AddBoolToObject(j, "quarantined", s.quarantined);
    cJSON_AddBoolToObject(j, "quarantine_retry_used", s.quarantine_retry_used);
    cJSON_AddNumberToObject(j, "reset_reason", s.reset_reason);
    cJSON_AddBoolToObject(j, "recovery_pending", s.recovery_pending);
    cJSON_AddNumberToObject(j, "recovery_attempts", s.recovery_attempts);
    cJSON_AddNumberToObject(j, "recovery_failures", s.recovery_failures);
    cJSON_AddNumberToObject(j, "recovery_at_ms", (double)s.recovery_at_ms);
    cJSON_AddStringToObject(j, "failure_stage", lighting_runtime_failure_stage_name(s.failure_stage));
    cJSON_AddStringToObject(j, "failure_reason", lighting_runtime_failure_reason_name(s.failure_reason));
    cJSON_AddNumberToObject(j, "failure_at_ms", (double)s.failure_at_ms);
    cJSON_AddNumberToObject(j, "failure_free_heap", s.failure_free_heap);
    cJSON_AddNumberToObject(j, "failure_largest_block", s.failure_largest_block);
    cJSON_AddNumberToObject(j, "required_free_heap", s.required_free_heap);
    cJSON_AddNumberToObject(j, "required_largest_block", s.required_largest_block);
    cJSON_AddNumberToObject(j, "provision_result", s.provision_result);
    cJSON_AddNumberToObject(j, "last_error", s.last_error);
    cJSON_AddNumberToObject(j, "sessions", s.secure_sessions);
    cJSON_AddNumberToObject(j, "free_heap", s.free_heap);
    cJSON_AddNumberToObject(j, "largest_block", s.largest_block);
    cJSON_AddBoolToObject(j, "worker_stack_sample_valid", s.worker_stack_sample_valid);
    cJSON_AddNumberToObject(j, "worker_stack_min_free_bytes", s.worker_stack_min_free_bytes);
    cJSON *local_time = cJSON_AddObjectToObject(j, "local_time");
    cJSON_AddStringToObject(local_time, "state", llt_state_name(s.local_time.state));
    cJSON_AddNumberToObject(local_time, "dns_replies", s.local_time.dns_replies);
    cJSON_AddNumberToObject(local_time, "ntp_replies", s.local_time.ntp_replies);
    cJSON_AddNumberToObject(local_time, "malformed", s.local_time.malformed);
    cJSON_AddNumberToObject(local_time, "foreign_peer", s.local_time.foreign_peer);
    cJSON_AddNumberToObject(local_time, "rate_limited", s.local_time.rate_limited);
    cJSON_AddNumberToObject(local_time, "clock_refused", s.local_time.clock_refused);
    cJSON_AddNumberToObject(local_time, "io_errors", s.local_time.io_errors);
    return j;
}
esp_err_t lights_provision(const cJSON *j)
{
    lgs_profile_t profiles[2] = {0};
    bool valid = cJSON_IsObject(j) && cJSON_GetArraySize(j) == 4 &&
        lower_hex(field(j, "owner_salt"), profiles[0].salt, 16) &&
        lower_hex(field(j, "owner_verifier"), profiles[0].verifier, 384) &&
        lower_hex(field(j, "guest_salt"), profiles[1].salt, 16) &&
        lower_hex(field(j, "guest_verifier"), profiles[1].verifier, 384);
    esp_err_t err = !valid ? ESP_ERR_INVALID_ARG : lighting_runtime_provision_trusted_admin(profiles);
    lgs_secure_zero(profiles, sizeof(profiles));
    return err;
}
esp_err_t lights_retry(void)
{
    if (!pause_mutex) return ESP_ERR_INVALID_STATE;
    /* Serialize retry with OTA/release quiescence. The runtime also checks the
     * pause latch at both allocation admission boundaries. */
    xSemaphoreTake(pause_mutex, portMAX_DELAY);
    esp_err_t result = lighting_runtime_retry_trusted_admin();
    xSemaphoreGive(pause_mutex);
    return result;
}

/* Reserve before SDK TLS allocation, rather than in HTTPS's post-handshake
 * callback. A busy peak fails transiently; callers retain their normal backoff.
 * Recursion permits the release worker's wider download/verification lease. */
int __real_esp_tls_server_session_create(esp_tls_cfg_server_t *, int, esp_tls_t *);
int __wrap_esp_tls_server_session_create(esp_tls_cfg_server_t *cfg, int fd, esp_tls_t *tls)
{
    if (!lights_peak_begin()) return -1;
    int result = __real_esp_tls_server_session_create(cfg, fd, tls);
    lights_peak_end(); return result;
}
int __real_esp_tls_conn_new_sync(const char *, int, int, const esp_tls_cfg_t *, esp_tls_t *);
int __wrap_esp_tls_conn_new_sync(const char *host, int size, int port,
                               const esp_tls_cfg_t *cfg, esp_tls_t *tls)
{
    if (!lights_peak_begin()) return -1;
    int result = __real_esp_tls_conn_new_sync(host, size, port, cfg, tls);
    lights_peak_end(); return result;
}
#else
esp_err_t lights_prepare(void) { return ESP_OK; }
void lights_poll_recovery(uint64_t now_ms) { (void)now_ms; }
esp_err_t lights_start(void) { return ESP_OK; }
bool lights_quiesce(void) { return true; }
void lights_resume(void) {}
bool lights_peak_begin(void) { return true; }
void lights_peak_end(void) {}
bool lights_relay_ready(void) { return false; }
bool lights_relay_message(const cJSON *j, bool *handled) { (void)j; *handled = false; return true; }
cJSON *lights_relay_next_reply(void) { return NULL; }
void lights_relay_disconnected(void) {}
cJSON *lights_status(void)
{
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "compiled", false); return j;
}
esp_err_t lights_provision(const cJSON *j) { (void)j; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t lights_retry(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif
