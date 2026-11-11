#include "arcade.h"
#include "arcade_address.h"
#include "protocol.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "font8x8.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static SemaphoreHandle_t lock;
static arcade_t state;
static nvs_handle_t storage;
static bool storage_ready;
static _Atomic bool guest_network, maintenance;
static _Atomic uint64_t heartbeat;
static httpd_handle_t server;
static uint8_t frame[ARCADE_FRAME_BYTES];
static bool frame_active;
static char boot_id[17];
static uint32_t log_index, log_overwrites;
static uint32_t pin;
static uint64_t pin_until, banner_until, next_knock, network_change_at;
static uint64_t pin_budget_start;
static unsigned pin_failures;
typedef struct {
    char token[33], receipt[ARCADE_RECEIPT_MAX];
    uint64_t id, until;
    bool remote;
} grant_t;
static grant_t grants[2];
typedef struct {
    uint32_t ip;
    bool remote;
    uint64_t start;
    unsigned requests, failures;
} peer_t;
static peer_t peers[8];
extern const uint8_t player_html[] asm("_binary_play_html_start");
extern const uint8_t player_html_end[] asm("_binary_play_html_end");
extern const uint8_t player_js[] asm("_binary_play_js_start");
extern const uint8_t player_js_end[] asm("_binary_play_js_end");
extern const uint8_t player_css[] asm("_binary_play_css_start");
extern const uint8_t player_css_end[] asm("_binary_play_css_end");
extern const uint8_t relay_js[] asm("_binary_relay_js_start");
extern const uint8_t relay_js_end[] asm("_binary_relay_js_end");
static uint64_t now_ms(void)
{
    return esp_timer_get_time() / 1000ULL;
}
bool arcade_guest_network(void)
{
    return guest_network && !maintenance;
}
void arcade_owner_release(void)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    arcade_release(&state, now_ms(), "Owner ended this turn");
    banner_until = pin_until = 0;
    xSemaphoreGive(lock);
}
void arcade_maintenance(void)
{
    maintenance = true;
}
esp_err_t arcade_prepare(void)
{
    lock = xSemaphoreCreateMutex();
    if (!lock)
        return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_open("arcade_v1", NVS_READWRITE, &storage);
    if (err != ESP_OK)
        return err;
    storage_ready = true;
    uint8_t enabled = 0;
    err = nvs_get_u8(storage, "guest_ap", &enabled);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        return err;
    guest_network = enabled == 1;
    err = nvs_get_u32(storage, "log_index", &log_index);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        return err;
    err = nvs_get_u32(storage, "overwrites", &log_overwrites);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        return err;
    random_hex(boot_id, 8);
    arcade_init(&state, esp_random(), now_ms());
    return ESP_OK;
}
esp_err_t arcade_set_guest_network(bool enabled)
{
    if (!storage_ready)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    if (ota_busy()) {
        xSemaphoreGive(operation_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = nvs_set_u8(storage, "guest_ap", enabled ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_commit(storage);
    if (err == ESP_OK) {
        guest_network = enabled;
        maintenance = false;
        network_change_at = now_ms() + 2000;
    }
    xSemaphoreGive(lock);
    xSemaphoreGive(operation_mutex);
    return err;
}
static void headers(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(
        r, "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src "
        "'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}
static esp_err_t json(httpd_req_t *r, cJSON *j, const char *status)
{
    if (!j)
        return ESP_ERR_NO_MEM;
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s)
        return ESP_ERR_NO_MEM;
    headers(r);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_status(r, status);
    esp_err_t e = httpd_resp_send(r, s, strlen(s));
    free(s);
    return e;
}
static esp_err_t error(httpd_req_t *r, const char *status, const char *code, const char *message)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", code);
    cJSON_AddStringToObject(j, "message", message);
    httpd_resp_set_hdr(r, "Connection", "close");
    json(r, j, status);
    return ESP_FAIL;
}
static bool same_origin(httpd_req_t *r)
{
    char host[64], origin[96];
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK ||
        strcmp(host, "192.168.4.1"))
        return false;
    if (r->method != HTTP_GET &&
        (httpd_req_get_hdr_value_str(r, "Origin", origin, sizeof(origin)) != ESP_OK ||
         strcmp(origin, "http://192.168.4.1")))
        return false;
    struct sockaddr_storage local;
    socklen_t n = sizeof(local);
    uint32_t ipv4;
    return getsockname(httpd_req_to_sockfd(r), (struct sockaddr *)&local, &n) == 0 &&
           arcade_address_ipv4(&local, n, &ipv4) && ipv4 == htonl(0xc0a80401u);
}
static peer_t *peer(uint32_t ipv4, bool remote, uint64_t now)
{
    peer_t *found = NULL, *oldest = &peers[0];
    for (unsigned i = 0; i < 8; i++) {
        if (peers[i].ip == ipv4 && peers[i].remote == remote)
            found = &peers[i];
        if (peers[i].start < oldest->start)
            oldest = &peers[i];
    }
    if (!found) {
        found = oldest;
        *found = (peer_t){.ip = ipv4, .remote = remote, .start = now};
    }
    if (now - found->start >= 10000) {
        found->start = now;
        found->requests = found->failures = 0;
    }
    if (++found->requests > 260)
        return NULL;
    return found;
}
static bool token_equal(const char *a, const char *b)
{
    if (strlen(a) != 32 || strlen(b) != 32)
        return false;
    unsigned difference = 0;
    for (unsigned i = 0; i < 32; i++)
        difference |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return difference == 0;
}
static grant_t *grant(const char *token)
{
    if (!token)
        return NULL;
    for (int i = 0; i < 2; i++)
        if (token_equal(token, grants[i].token) && now_ms() < grants[i].until)
            return &grants[i];
    return NULL;
}
static cJSON *status_locked(uint64_t id)
{
    static const char *stages[] = {"idle", "menu", "countdown", "play", "result"};
    cJSON *j = cJSON_CreateObject();
    int slot = arcade_player(&state, id);
    cJSON_AddNumberToObject(j, "protocol", 1);
    cJSON_AddStringToObject(j, "boot", boot_id);
    cJSON_AddNumberToObject(j, "uptime_ms", state.now);
    cJSON_AddStringToObject(j, "stage", stages[state.stage]);
    cJSON_AddNumberToObject(j, "slot", slot);
    cJSON_AddNumberToObject(j, "game", state.game);
    cJSON_AddStringToObject(j, "reason", state.reason);
    cJSON_AddNumberToObject(j, "revision", state.revision);
    cJSON_AddNumberToObject(j, "turn_ms",
                            state.turn_until > state.now ? state.turn_until - state.now : 0);
    cJSON_AddNumberToObject(
        j, "cooldown_ms", state.cooldown_until > state.now ? state.cooldown_until - state.now : 0);
    cJSON_AddNumberToObject(j, "phase_ms",
                            state.phase_until > state.now ? state.phase_until - state.now : 0);
    cJSON_AddBoolToObject(j, "updating", ota_busy());
    if (slot >= 0) {
        arcade_player_t *p = &state.players[slot];
        unsigned limit = state.stage == ARCADE_MENU ? ARCADE_MENU_IDLE_MS : ARCADE_IDLE_MS;
        uint64_t idle = state.now - p->last_action;
        cJSON_AddNumberToObject(j, "idle_ms", idle < limit ? limit - idle : 0);
        cJSON_AddNumberToObject(j, "sequence", p->sequence);
    }
    cJSON *players = cJSON_AddArrayToObject(j, "players");
    for (int i = 0; i < 2; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject(p, "present", state.players[i].id != 0);
        cJSON_AddBoolToObject(p, "ready", state.players[i].ready);
        cJSON_AddItemToArray(players, p);
    }
    if (state.game == ARCADE_PONG && state.stage != ARCADE_MENU && state.stage != ARCADE_IDLE) {
        cJSON *p = cJSON_AddObjectToObject(j, "pong");
        cJSON_AddBoolToObject(p, "ai", state.game_state.pong.ai);
        cJSON_AddNumberToObject(p, "p1", state.game_state.pong.points[0]);
        cJSON_AddNumberToObject(p, "p2", state.game_state.pong.points[1]);
    }
    return j;
}
static cJSON *read_body(httpd_req_t *r)
{
    if (r->content_len <= 0 || r->content_len > 1024)
        return NULL;
    char type[64];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strncmp(type, "application/json", 16))
        return NULL;
    char body[1025];
    size_t at = 0;
    while (at < r->content_len) {
        int n = httpd_req_recv(r, body + at, r->content_len - at);
        if (n <= 0)
            return NULL;
        at += n;
    }
    body[at] = 0;
    return protocol_object(body, at);
}
static bool integer(const cJSON *j, const char *key, double lo, double hi, int64_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < lo ||
        v->valuedouble > hi || floor(v->valuedouble) != v->valuedouble)
        return false;
    *out = (int64_t)v->valuedouble;
    return true;
}
static bool save_receipt(const char *receipt)
{
    char key[12];
    snprintf(key, sizeof(key), "visit%u", (unsigned)(log_index % 8));
    esp_err_t err = nvs_set_str(storage, key, receipt);
    if (err == ESP_OK)
        err = nvs_set_u32(storage, "log_index", log_index + 1);
    if (err == ESP_OK && log_index >= 8)
        err = nvs_set_u32(storage, "overwrites", log_overwrites + 1);
    if (err == ESP_OK)
        err = nvs_commit(storage);
    if (err != ESP_OK)
        return false;
    if (log_index >= 8)
        log_overwrites++;
    log_index++;
    return true;
}
static cJSON *dispatch_error(int *status, const char *http_status, const char *code,
                             const char *message)
{
    *status = atoi(http_status);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", code);
    cJSON_AddStringToObject(j, "message", message);
    return j;
}
static cJSON *dispatch_locked(const char *op, const char *token, const cJSON *body, peer_t *p,
                              bool remote, int *status)
{
    uint64_t now = now_ms();
    arcade_tick(&state, now);
    if (!strcmp(op, "status")) {
        grant_t *g = grant(token);
        int slot = g ? arcade_player(&state, g->id) : -1;
        if (slot >= 0)
            state.players[slot].last_seen = now;
        return status_locked(g ? g->id : 0);
    }
    if (!strcmp(op, "receipt")) {
        grant_t *g = grant(token);
        if (!g)
            return dispatch_error(status, "401", "session", "That visit has ended.");
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "receipt", g->receipt);
        return j;
    }
    if (!body)
        return dispatch_error(status, "400", "body", "Invalid or oversized request.");
    if (ota_busy() || maintenance)
        return dispatch_error(status, "409", "updating",
                              "The display is unavailable. Please wait.");
    if (!strcmp(op, "knock")) {
        if (state.stage != ARCADE_IDLE && state.stage != ARCADE_MENU) {
            return dispatch_error(status, "409 Conflict", "playing",
                                  "A match is in progress. Join when the menu returns.");
        }
        if (now >= pin_until) {
            pin = 100000 + esp_random() % 900000;
            pin_until = now + 60000;
        }
        if (now >= next_knock) {
            banner_until = now + 5000;
            next_knock = now + 60000;
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        cJSON_AddNumberToObject(j, "expires_ms", pin_until - now);
        return j;
    }
    if (!strcmp(op, "join")) {
        int64_t code = 0;
        const cJSON *profile = cJSON_GetObjectItemCaseSensitive(body, "profile");
        if (now - pin_budget_start >= 60000) {
            pin_budget_start = now;
            pin_failures = 0;
        }
        bool valid = pin_failures < 15 && p->failures < 5 &&
                     integer(body, "code", 100000, 999999, &code) && now < pin_until &&
                     code == pin && cJSON_IsString(profile) &&
                     strlen(profile->valuestring) <= ARCADE_PROFILE_MAX;
        if (!valid) {
            p->failures++;
            pin_failures++;
            return dispatch_error(status, "403 Forbidden", "code",
                                  "Read the current six-digit code on the display and try again.");
        }
        if ((state.stage != ARCADE_IDLE && state.stage != ARCADE_MENU) || state.players[1].id ||
            now < state.cooldown_until) {
            return dispatch_error(status, "409 Conflict", "full",
                                  "The stage is busy. Please wait for the next turn.");
        }
        cJSON *decoded = NULL;
        if (!arcade_profile_validate(profile->valuestring, &decoded)) {
            return dispatch_error(
                status, "400 Bad Request", "profile",
                "Your pass is invalid or expired. Use a name and room number to join as a guest.");
        }
        cJSON_Delete(decoded);
        int slot = arcade_player(&state, grants[0].id) < 0 ? 0 : 1;
        grant_t *g = &grants[slot];
        memset(g, 0, sizeof(*g));
        random_hex(g->token, 16);
        esp_fill_random(&g->id, sizeof(g->id));
        if (!g->id)
            g->id = 1;
        g->until = now + ARCADE_TURN_MS + 60000;
        g->remote = remote;
        cJSON *receipt = cJSON_CreateObject();
        char visit[33];
        random_hex(visit, 16);
        cJSON_AddNumberToObject(receipt, "v", 1);
        cJSON_AddStringToObject(receipt, "visit", visit);
        cJSON_AddStringToObject(receipt, "device", identity.device_id);
        cJSON_AddStringToObject(receipt, "boot", boot_id);
        cJSON_AddNumberToObject(receipt, "uptime_ms", now);
        cJSON_AddStringToObject(receipt, "profile", profile->valuestring);
        char *payload = cJSON_PrintUnformatted(receipt);
        cJSON_Delete(receipt);
        bool signed_ok = payload && arcade_receipt_sign(payload, g->receipt);
        free(payload);
        bool logged = signed_ok && save_receipt(g->receipt);
        if (!logged) {
            memset(g, 0, sizeof(*g));
            return dispatch_error(
                status, "503 Service Unavailable", "log",
                "The private visitor log could not be saved. Please ask the owner to "
                "check the display.");
        }
        enum arcade_result joined = arcade_join(&state, g->id, now);
        if (joined != ARCADE_OK) {
            memset(g, 0, sizeof(*g));
            return dispatch_error(status, "409 Conflict", "busy",
                                  "The stage changed. Try joining again.");
        }
        banner_until = pin_until = 0;
        next_knock = 0; /* A second player gets a fresh one-use code. */
        cJSON *j = status_locked(g->id);
        cJSON_AddStringToObject(j, "token", g->token);
        cJSON_AddStringToObject(j, "receipt", g->receipt);
        return j;
    }
    if (!strcmp(op, "command")) {
        grant_t *g = grant(token);
        int64_t seq, action, value, axis;
        bool valid = g && integer(body, "sequence", 0, UINT32_MAX, &seq) &&
                     integer(body, "action", 0, ARCADE_HEARTBEAT, &action) &&
                     integer(body, "value", 0, 127, &value) &&
                     integer(body, "axis", -1000, 1000, &axis);
        enum arcade_result result =
            valid ? arcade_command(&state, g->id, seq, action, value, axis, now) : ARCADE_INVALID;
        cJSON *j = result == ARCADE_OK ? status_locked(g->id) : NULL;
        if (result != ARCADE_OK)
            return dispatch_error(
                status, result == ARCADE_RATE_LIMITED ? "429 Too Many Requests" : "409 Conflict",
                "command", "That action is unavailable. Refresh the controller state.");
        return j;
    }
    return dispatch_error(status, "404 Not Found", "not_found", "Unknown arcade operation.");
}

cJSON *arcade_dispatch(const char *op, const char *token, const cJSON *body, uint32_t peer_id,
                       bool remote, int *status)
{
    *status = 200;
    xSemaphoreTake(lock, portMAX_DELAY);
    grant_t *g = remote ? grant(token) : NULL;
    /* Campus NAT can put both human controllers behind one public IP. Keep
       admission budgets per network, but authenticated traffic per grant. */
    if (g && (!strcmp(op, "command") || !strcmp(op, "status") || !strcmp(op, "receipt")))
        peer_id = (uint32_t)g->id ^ (uint32_t)(g->id >> 32);
    peer_t *p = peer(peer_id, remote, now_ms());
    cJSON *j = p ? dispatch_locked(op, token, body, p, remote, status)
                 : dispatch_error(status, "429", "rate", "Please slow down and retry shortly.");
    xSemaphoreGive(lock);
    return j;
}
void arcade_relay_disconnected(void)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (unsigned i = 0; i < 2; i++) {
        int slot = grants[i].remote ? arcade_player(&state, grants[i].id) : -1;
        if (slot >= 0)
            state.players[slot].held = state.players[slot].axis = 0;
    }
    xSemaphoreGive(lock);
}
const char *arcade_boot_id(void)
{
    return boot_id;
}
static esp_err_t route(httpd_req_t *r)
{
    if (!same_origin(r))
        return error(r, "403 Forbidden", "origin",
                     "Open http://192.168.4.1/play on the display Wi-Fi.");
    struct sockaddr_storage remote;
    socklen_t n = sizeof(remote);
    uint32_t ipv4;
    if (getpeername(httpd_req_to_sockfd(r), (struct sockaddr *)&remote, &n) ||
        !arcade_address_ipv4(&remote, n, &ipv4))
        return error(r, "403 Forbidden", "peer", "Unknown controller address.");
    if (r->method == HTTP_GET) {
        const uint8_t *data = NULL, *end = NULL;
        const char *mime = NULL;
        if (!strcmp(r->uri, "/play") || !strcmp(r->uri, "/")) {
            data = player_html;
            end = player_html_end;
            mime = "text/html; charset=utf-8";
        } else if (!strcmp(r->uri, "/play.js")) {
            data = player_js;
            end = player_js_end;
            mime = "text/javascript; charset=utf-8";
        } else if (!strcmp(r->uri, "/relay.js")) {
            data = relay_js;
            end = relay_js_end;
            mime = "text/javascript; charset=utf-8";
        } else if (!strcmp(r->uri, "/play.css")) {
            data = player_css;
            end = player_css_end;
            mime = "text/css; charset=utf-8";
        }
        if (data) {
            xSemaphoreTake(lock, portMAX_DELAY);
            bool allowed = peer(ipv4, false, now_ms()) != NULL;
            xSemaphoreGive(lock);
            if (!allowed)
                return error(r, "429 Too Many Requests", "rate", "Please slow down.");
            headers(r);
            httpd_resp_set_type(r, mime);
            return httpd_resp_send(r, (const char *)data, end - data);
        }
    }
    if (strncmp(r->uri, "/arcade/", 8))
        return error(r, "404 Not Found", "not_found", "Open /play for the controller.");
    const char *op = r->uri + 8;
    bool read = !strcmp(op, "status") || !strcmp(op, "receipt");
    if ((read && r->method != HTTP_GET) || (!read && r->method != HTTP_POST))
        return error(r, "405 Method Not Allowed", "method", "Unsupported method.");
    cJSON *body = r->method == HTTP_POST ? read_body(r) : NULL;
    char token[40] = {0};
    httpd_req_get_hdr_value_str(r, "X-Arcade-Token", token, sizeof(token));
    int code;
    cJSON *j = arcade_dispatch(op, token, body, ipv4, false, &code);
    cJSON_Delete(body);
    const char *status = code == 200   ? "200 OK"
                         : code == 400 ? "400 Bad Request"
                         : code == 401 ? "401 Unauthorized"
                         : code == 403 ? "403 Forbidden"
                         : code == 404 ? "404 Not Found"
                         : code == 409 ? "409 Conflict"
                         : code == 429 ? "429 Too Many Requests"
                                       : "503 Service Unavailable";
    return json(r, j, status);
}
static void task(void *unused)
{
    (void)unused;
    for (;;) {
        uint64_t now = now_ms();
        heartbeat = now;
        bool change = false;
        xSemaphoreTake(lock, portMAX_DELAY);
        if (ota_busy() && state.stage != ARCADE_IDLE)
            arcade_release(&state, now, "Display updating");
        arcade_tick(&state, now);
        if (network_change_at && now >= network_change_at && !ota_busy()) {
            network_change_at = 0;
            change = true;
        }
        xSemaphoreGive(lock);
        if (change) {
            xSemaphoreTake(operation_mutex, portMAX_DELAY);
            if (ota_busy()) {
                xSemaphoreTake(lock, portMAX_DELAY);
                network_change_at = now_ms() + 2000;
                xSemaphoreGive(lock);
            } else if (net_refresh_ap() != ESP_OK) {
                diag_event("Arcade hotspot configuration failed; maintenance required");
                net_maintenance();
            }
            xSemaphoreGive(operation_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t arcade_start(void)
{
    if (!storage_ready)
        return ESP_ERR_INVALID_STATE;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    /* Keep the HTTP default control port. HTTPS reserves default + 1. */
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 2;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 10240;
    cfg.task_priority = tskIDLE_PRIORITY + 1;
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 2;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK)
        return err;
    httpd_method_t methods[] = {HTTP_GET, HTTP_POST};
    for (unsigned i = 0; i < 2; i++) {
        httpd_uri_t uri = {.uri = "/*", .method = methods[i], .handler = route};
        err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK)
            return err;
    }
    heartbeat = now_ms();
    return xTaskCreate(task, "arcade", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
bool arcade_healthy(void)
{
    return server && now_ms() - heartbeat < 5000;
}
bool arcade_display_active(void)
{
    if (!lock)
        return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool active = state.stage != ARCADE_IDLE || now_ms() < banner_until;
    xSemaphoreGive(lock);
    return active;
}
bool arcade_frame_begin(void)
{
    if (!lock)
        return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    frame_active = state.stage != ARCADE_IDLE;
    if (frame_active)
        arcade_render(&state, frame);
    xSemaphoreGive(lock);
    return frame_active;
}
bool arcade_frame_row(unsigned w, unsigned h, unsigned y, uint16_t *row)
{
    if (!frame_active || y >= h)
        return false;
    unsigned dw = w, dh = w * 128 / 160;
    if (dh > h) {
        dh = h;
        dw = h * 160 / 128;
    }
    unsigned left = (w - dw) / 2, top = (h - dh) / 2;
    for (unsigned x = 0; x < w; x++) {
        uint8_t c = 0;
        if (x >= left && x < left + dw && y >= top && y < top + dh) {
            unsigned at = ((y - top) * 128 / dh) * 160 + (x - left) * 160 / dw;
            c = (at & 1) ? frame[at / 2] & 15 : frame[at / 2] >> 4;
        }
        row[x] = arcade_color(c);
    }
    return true;
}
bool arcade_pair_row(unsigned y, uint16_t *row, unsigned w)
{
    if (!lock || y >= 40)
        return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool show = now_ms() < banner_until;
    uint32_t code = pin;
    xSemaphoreGive(lock);
    if (!show)
        return false;
    char caption[24];
    snprintf(caption, sizeof(caption), y < 16 ? "JOIN THE ARCADE" : "%06u", (unsigned)code);
    unsigned scale = y < 16 ? 1 : 2, cy = y < 16 ? y - 4 : y - 17;
    size_t count = strlen(caption);
    int left = ((int)w - (int)count * 8 * (int)scale) / 2;
    for (unsigned x = 0; x < w; x++) {
        row[x] = 0x0862;
        int px = (int)x - left;
        if (px >= 0 && cy < 8 * scale && (unsigned)px < count * 8 * scale) {
            unsigned ch = (unsigned char)caption[px / (8 * scale)];
            if (font8x8_basic[ch][cy / scale] & (1 << ((px / scale) % 8)))
                row[x] = y < 16 ? 0xff9a : 0xff00;
        }
    }
    return true;
}
cJSON *arcade_status(void)
{
    if (!lock)
        return cJSON_CreateObject();
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON *j = status_locked(0);
    cJSON_AddBoolToObject(j, "guest_network", arcade_guest_network());
    cJSON_AddBoolToObject(j, "healthy", arcade_healthy());
    cJSON_AddNumberToObject(j, "local_visits", log_index < 8 ? log_index : 8);
    cJSON_AddNumberToObject(j, "overwritten_visits", log_overwrites);
    xSemaphoreGive(lock);
    return j;
}
cJSON *arcade_logs(void)
{
    cJSON *j = cJSON_CreateArray();
    if (!storage_ready)
        return j;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (unsigned i = 0; i < 8 && i < log_index; i++) {
        char key[12], receipt[ARCADE_RECEIPT_MAX];
        size_t size = sizeof(receipt);
        snprintf(key, sizeof(key), "visit%u", (unsigned)((log_index - 1 - i) % 8));
        if (nvs_get_str(storage, key, receipt, &size) == ESP_OK)
            cJSON_AddItemToArray(j, cJSON_CreateString(receipt));
    }
    xSemaphoreGive(lock);
    return j;
}

cJSON *arcade_pairing(void)
{
    cJSON *j = cJSON_CreateObject();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (now_ms() < pin_until)
        cJSON_AddNumberToObject(j, "code", pin);
    xSemaphoreGive(lock);
    return j;
}

cJSON *arcade_public_status(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON *j = status_locked(0);
    xSemaphoreGive(lock);
    return j;
}
