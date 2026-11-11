#include "relay.h"
#include "relay_frame.h"
#include "arcade.h"
#include "protocol.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    uint32_t version;
    uint8_t enabled;
    char host[128];
} relay_config_t;
typedef struct {
    size_t length;
    char text[RELAY_INPUT_MAX + 1];
} relay_message_t;
static relay_config_t config;
static SemaphoreHandle_t mutex;
static QueueHandle_t incoming;
static nvs_handle_t storage;
static bool storage_ready, auth_sent;
static _Atomic bool enabled, connected, authenticated, fatal, reconfigure;
static _Atomic uint32_t connects, disconnects, rejected;
static _Atomic unsigned phase;
static _Atomic unsigned pause_users;
static _Atomic bool client_live;
static _Atomic int last_tls_error, last_tls_stack_error, last_tls_flags, last_http_status,
    last_socket_error;
static uint64_t change_at;
static relay_frame_t frame;
static relay_message_t assembled;
static uint64_t milliseconds(void)
{
    return esp_timer_get_time() / 1000ULL;
}
bool relay_enabled(void)
{
    return enabled;
}
bool relay_quiesce(void)
{
    if (!mutex)
        return true;
    xSemaphoreTake(mutex, portMAX_DELAY);
    pause_users++;
    xSemaphoreGive(mutex);
    uint64_t deadline = milliseconds() + 7000;
    while (client_live && milliseconds() < deadline)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (!client_live)
        return true;
    relay_resume();
    return false;
}
void relay_resume(void)
{
    if (!mutex)
        return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (pause_users)
        pause_users--;
    xSemaphoreGive(mutex);
}
static bool host_valid(const char *host)
{
    size_t n = host ? strlen(host) : 0;
    if (!n || n >= sizeof(config.host) || host[0] == '.' || host[n - 1] == '.')
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((host[i] >= 'a' && host[i] <= 'z') || (host[i] >= '0' && host[i] <= '9') ||
              host[i] == '-' || host[i] == '.'))
            return false;
    return strchr(host, '.') != NULL;
}
esp_err_t relay_prepare(void)
{
    mutex = xSemaphoreCreateMutex();
    if (!mutex)
        return ESP_ERR_NO_MEM;
    esp_err_t e = nvs_open("relay_v1", NVS_READWRITE, &storage);
    if (e != ESP_OK)
        return e;
    storage_ready = true;
    size_t n = sizeof(config);
    e = nvs_get_blob(storage, "config", &config, &n);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        config = (relay_config_t){.version = 1};
        return ESP_OK;
    }
    if (e != ESP_OK)
        return e;
    if (n != sizeof(config) || config.version != 1 || config.enabled > 1 ||
        !memchr(config.host, 0, sizeof(config.host)) ||
        (config.enabled && !host_valid(config.host)))
        return ESP_ERR_INVALID_STATE;
    enabled = config.enabled;
    return ESP_OK;
}
esp_err_t relay_configure(const char *host, bool on)
{
    if (!storage_ready || !mutex || !host || (*host && !host_valid(host)) || (on && !*host))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    if (ota_busy()) {
        xSemaphoreGive(operation_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    relay_config_t next = {.version = 1, .enabled = on};
    strlcpy(next.host, host, sizeof(next.host));
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t e = nvs_set_blob(storage, "config", &next, sizeof(next));
    if (e == ESP_OK)
        e = nvs_commit(storage);
    if (e == ESP_OK) {
        config = next;
        enabled = on;
        change_at = milliseconds() + 2000;
        reconfigure = true;
    }
    xSemaphoreGive(mutex);
    xSemaphoreGive(operation_mutex);
    return e;
}
static void event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = event_data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        connected = true;
        connects++;
        return;
    }
    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED ||
        id == WEBSOCKET_EVENT_ERROR) {
        /* 1.8.0 assigns transport error fields on the subsequent DISCONNECTED
           event. ERROR may precede error_type and leave those fields unset. */
        if (d->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            last_tls_error = d->error_handle.esp_tls_last_esp_err;
            last_tls_stack_error = d->error_handle.esp_tls_stack_err;
            last_tls_flags = d->error_handle.esp_tls_cert_verify_flags;
            last_http_status = d->error_handle.esp_ws_handshake_status_code;
            last_socket_error = d->error_handle.esp_transport_sock_errno;
        }
        if (id == WEBSOCKET_EVENT_ERROR && d->data_ptr && d->data_len > 0) {
            char line[128];
            int n = d->data_len < 110 ? d->data_len : 110;
            /* The pinned component emits transport error codes here, not payloads. */
            snprintf(line, sizeof(line), "Relay: %.*s", n, d->data_ptr);
            diag_event(line);
        }
        connected = authenticated = false;
        fatal = true;
        return;
    }
#if WS_TRANSPORT_HEADER_CALLBACK_SUPPORT
    if (id == WEBSOCKET_EVENT_HEADER_RECEIVED && d->data_len >= 9 &&
        !strncasecmp(d->data_ptr, "Location:", 9)) {
        fatal = true;
        phase = 7;
        return;
    }
#endif
    if (id != WEBSOCKET_EVENT_DATA || fatal)
        return;
    int done = relay_frame_feed(&frame, d->op_code, d->fin, d->payload_len, d->payload_offset,
                                d->data_ptr, d->data_len);
    if (done < 0) {
        fatal = true;
        rejected++;
        return;
    }
    if (done == 1) {
        assembled.length = frame.used;
        memcpy(assembled.text, frame.bytes, frame.used + 1);
        if (xQueueSend(incoming, &assembled, 0) != pdTRUE) {
            fatal = true;
            rejected++;
        }
    }
}
static const char *text(const cJSON *j, const char *key, size_t max)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) && strlen(v->valuestring) <= max ? v->valuestring : NULL;
}
static bool number(const cJSON *j, const char *key, double lo, double hi, uint64_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        floor(v->valuedouble) != v->valuedouble || v->valuedouble < lo || v->valuedouble > hi)
        return false;
    *out = (uint64_t)v->valuedouble;
    return true;
}
static bool send_json(esp_websocket_client_handle_t client, cJSON *j)
{
    if (!j)
        return false;
    char *bytes = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!bytes)
        return false;
    size_t n = strlen(bytes);
    bool ok = n <= 4096 && !fatal &&
              esp_websocket_client_send_text(client, bytes, (int)n, pdMS_TO_TICKS(250)) == (int)n;
    free(bytes);
    return ok;
}
static bool message(esp_websocket_client_handle_t client, const relay_message_t *msg)
{
    cJSON *j = protocol_object(msg->text, msg->length);
    if (!j)
        return false;
    uint64_t version = 0;
    const char *type = text(j, "type", 20);
    bool ok = number(j, "v", 1, 1, &version) && type;
    if (!ok) {
        cJSON_Delete(j);
        return false;
    }
    if (!strcmp(type, "challenge") && !authenticated && !auth_sent) {
        const char *nonce = text(j, "nonce", 64), *connection = text(j, "connection", 32);
        uint8_t scratch[32];
        char message[220], signature[161];
        ok = nonce && connection && hex_decode(nonce, scratch, 32) &&
             hex_decode(connection, scratch, 16);
        if (ok) {
            int n = snprintf(message, sizeof(message), "203-relay-device-v1\n%s\n%s\n%s\n%s",
                             identity.device_id, nonce, connection, arcade_boot_id());
            ok = n > 0 && (size_t)n < sizeof(message) && arcade_device_sign(message, signature);
        }
        if (ok) {
            cJSON *reply = cJSON_CreateObject();
            cJSON_AddNumberToObject(reply, "v", 1);
            cJSON_AddStringToObject(reply, "type", "authenticate");
            cJSON_AddStringToObject(reply, "device", identity.device_id);
            cJSON_AddStringToObject(reply, "boot", arcade_boot_id());
            cJSON_AddStringToObject(reply, "signature", signature);
            ok = send_json(client, reply);
            auth_sent = ok;
        }
    } else if (!strcmp(type, "authenticated") && auth_sent && !authenticated) {
        authenticated = true;
        phase = 4;
        diag_event("Internet game relay authenticated");
    } else if (!strcmp(type, "request") && authenticated) {
        const char *op = text(j, "op", 12), *token = text(j, "token", 32),
                   *body = text(j, "body", 1024), *connection = text(j, "connection", 32),
                   *boot = text(j, "boot", 16);
        uint64_t id = 0, peer = 0, lease = 0;
        uint8_t scratch[16];
        ok = op && token && body && connection && boot && !strcmp(boot, arcade_boot_id()) &&
             hex_decode(connection, scratch, 16) && number(j, "id", 1, UINT32_MAX, &id) &&
             number(j, "peer", 1, UINT32_MAX, &peer) &&
             number(j, "lease", 0, 9007199254740991.0, &lease) &&
             (!strcmp(op, "status") || !strcmp(op, "receipt") || !strcmp(op, "knock") ||
              !strcmp(op, "join") || !strcmp(op, "command"));
        if (ok) {
            cJSON *payload = protocol_object(body, strlen(body)), *result = NULL;
            int status = 400;
            uint64_t now = milliseconds();
            if (!strcmp(op, "command") && (!lease || lease > now || now - lease > 1000)) {
                result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "error", "stale");
                cJSON_AddStringToObject(result, "message",
                                        "The connection paused. Controls were released.");
                status = 409;
            } else if (payload)
                result = arcade_dispatch(op, token, payload, (uint32_t)peer, true, &status);
            cJSON_Delete(payload);
            if (result) {
                char *data = cJSON_PrintUnformatted(result);
                cJSON_Delete(result);
                cJSON *reply = cJSON_CreateObject();
                cJSON_AddNumberToObject(reply, "v", 1);
                cJSON_AddStringToObject(reply, "type", "reply");
                cJSON_AddStringToObject(reply, "connection", connection);
                cJSON_AddNumberToObject(reply, "id", (double)id);
                cJSON_AddNumberToObject(reply, "status", status);
                cJSON_AddStringToObject(reply, "body", data ? data : "{}");
                free(data);
                ok = send_json(client, reply);
            } else
                ok = false;
        }
    } else
        ok = false;
    cJSON_Delete(j);
    return ok;
}
static void task(void *arg)
{
    (void)arg;
    esp_websocket_client_handle_t client = NULL;
    /* The websocket transport retains this pointer for the client's lifetime. */
    char client_host[sizeof(config.host)] = {0};
    unsigned failures = 0;
    uint64_t next = 0, started = 0, pulse = 0, public_at = 0;
    relay_message_t *msg = malloc(sizeof(*msg));
    if (!msg) {
        phase = 8;
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        uint64_t now = milliseconds();
        bool apply = false;
        relay_config_t snapshot;
        xSemaphoreTake(mutex, portMAX_DELAY);
        snapshot = config;
        if (reconfigure && now >= change_at) {
            reconfigure = false;
            apply = true;
        }
        xSemaphoreGive(mutex);
        bool ready =
            enabled && !pause_users && net_online() && time(NULL) >= 1700000000 && !ota_busy();
        if (client && (!ready || fatal || apply || (!authenticated && now - started > 15000))) {
            authenticated = connected = false;
            arcade_relay_disconnected();
            esp_websocket_client_stop(client);
            esp_websocket_client_destroy(client);
            client = NULL;
            client_live = false;
            xQueueReset(incoming);
            memset(&frame, 0, sizeof(frame));
            disconnects++;
            next = now + (1000ULL << (failures < 6 ? failures++ : 6)) + (esp_random() % 1000);
            phase = 5;
        }
        if (apply) {
            bool acquired = xSemaphoreTake(operation_mutex, 0) == pdTRUE;
            if (!acquired || ota_busy()) {
                xSemaphoreTake(mutex, portMAX_DELAY);
                reconfigure = true;
                change_at = milliseconds() + 2000;
                xSemaphoreGive(mutex);
            } else {
                net_apply();
            }
            if (acquired)
                xSemaphoreGive(operation_mutex);
            next = now + 1000;
        }
        if (!enabled)
            phase = 0;
        else if (!net_online())
            phase = 1;
        else if (time(NULL) < 1700000000)
            phase = 2;
        else if (ota_busy() || pause_users)
            phase = 6;
        if (!client && ready && now >= next && !apply) {
            xSemaphoreTake(mutex, portMAX_DELAY);
            bool permitted = !pause_users && !ota_busy();
            if (permitted)
                client_live = true;
            xSemaphoreGive(mutex);
            if (!permitted)
                continue;
            char nonce[33], proof[161], proof_message[180], client_headers[384];
            random_hex(nonce, 16);
            long long unix_time = (long long)time(NULL);
            snprintf(proof_message, sizeof(proof_message), "203-relay-connect-v1\n%s\n%s\n%lld\n%s",
                     identity.device_id, arcade_boot_id(), unix_time, nonce);
            if (!arcade_device_sign(proof_message, proof)) {
                client_live = false;
                phase = 8;
                next = now + 30000;
                continue;
            }
            snprintf(client_headers, sizeof(client_headers),
                     "X-203-Boot: %s\r\nX-203-Time: %lld\r\nX-203-Nonce: %s\r\nX-203-Proof: %s\r\n",
                     arcade_boot_id(), unix_time, nonce, proof);
            char uri[180];
            strlcpy(client_host, snapshot.host, sizeof(client_host));
            snprintf(uri, sizeof(uri), "wss://%s/v1/device", client_host);
            esp_websocket_client_config_t cfg = {.uri = uri,
                                                 .headers = client_headers,
                                                 .cert_common_name = client_host,
                                                 .crt_bundle_attach = esp_crt_bundle_attach,
                                                 .disable_auto_reconnect = true,
                                                 .task_prio = tskIDLE_PRIORITY + 1,
                                                 .task_stack = 6144,
                                                 .buffer_size = 1024,
                                                 .network_timeout_ms = 5000,
                                                 .ping_interval_sec = 10,
                                                 .pingpong_timeout_sec = 10,
                                                 .keep_alive_enable = true,
                                                 .user_agent = "203-Arcade/1"};
            fatal = false;
            auth_sent = false;
            authenticated = connected = false;
            phase = 3;
            started = now;
            client = esp_websocket_client_init(&cfg);
            if (!client) {
                client_live = false;
                phase = 8;
                next = now + 30000;
            } else if (esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, event, NULL) !=
                           ESP_OK ||
                       esp_websocket_client_start(client) != ESP_OK)
                fatal = true;
        }
        if (client && !fatal && xQueueReceive(incoming, msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (!message(client, msg)) {
                fatal = true;
                rejected++;
            }
        }
        if (client && authenticated && !fatal && now >= pulse) {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddNumberToObject(j, "v", 1);
            cJSON_AddStringToObject(j, "type", "pulse");
            cJSON_AddNumberToObject(j, "lease", (double)milliseconds());
            if (now >= public_at) {
                cJSON *view = arcade_public_status();
                char *data = cJSON_PrintUnformatted(view);
                cJSON_Delete(view);
                if (data)
                    cJSON_AddStringToObject(j, "state", data);
                free(data);
                public_at = now + 1000;
            }
            if (!send_json(client, j))
                fatal = true;
            pulse = now + 250;
            failures = 0;
        }
        if (!client || fatal)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
}
esp_err_t relay_start(void)
{
    if (!storage_ready)
        return ESP_ERR_INVALID_STATE;
    incoming = xQueueCreate(4, sizeof(relay_message_t));
    if (!incoming)
        return ESP_ERR_NO_MEM;
    return xTaskCreate(task, "arcade_relay", 8192, NULL, tskIDLE_PRIORITY + 1, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
cJSON *relay_status(void)
{
    static const char *names[] = {
        "disabled", "waiting_for_wifi",  "waiting_for_time",  "connecting",    "online",
        "retrying", "paused_for_update", "redirect_rejected", "resource_error"};
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", enabled);
    cJSON_AddBoolToObject(j, "connected", authenticated && !fatal);
    unsigned p = phase;
    cJSON_AddStringToObject(j, "state", p < 9 ? names[p] : "error");
    cJSON_AddNumberToObject(j, "connections", connects);
    cJSON_AddNumberToObject(j, "disconnects", disconnects);
    cJSON_AddNumberToObject(j, "rejected_messages", rejected);
    cJSON_AddNumberToObject(j, "last_tls_error", last_tls_error);
    cJSON_AddNumberToObject(j, "last_tls_stack_error", last_tls_stack_error);
    cJSON_AddNumberToObject(j, "last_tls_flags", last_tls_flags);
    cJSON_AddNumberToObject(j, "last_http_status", last_http_status);
    cJSON_AddNumberToObject(j, "last_socket_error", last_socket_error);
    if (mutex) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        cJSON_AddStringToObject(j, "host", config.host);
        xSemaphoreGive(mutex);
    }
    return j;
}
