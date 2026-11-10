#include "entrance.h"
#include "protocol.h"
#include "auth.h"
#include "esp_https_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static httpd_handle_t server;
static auth_state_t auth_policy;
extern const uint8_t html_start[] asm("_binary_index_html_start");
extern const uint8_t html_end[] asm("_binary_index_html_end");
extern const uint8_t js_start[] asm("_binary_app_js_start");
extern const uint8_t js_end[] asm("_binary_app_js_end");
extern const uint8_t css_start[] asm("_binary_style_css_start");
extern const uint8_t css_end[] asm("_binary_style_css_end");

static void headers(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(
        r, "Content-Security-Policy",
        "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
        "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}
static esp_err_t json_response(httpd_req_t *r, cJSON *json, const char *status)
{
    if (!json)
        return ESP_ERR_NO_MEM;
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body)
        return ESP_ERR_NO_MEM;
    headers(r);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_status(r, status);
    esp_err_t err = httpd_resp_send(r, body, strlen(body));
    free(body);
    return err;
}
static esp_err_t error_response(httpd_req_t *r, const char *status, const char *code,
                                const char *message)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", code);
    cJSON_AddStringToObject(j, "message", message);
    httpd_resp_set_hdr(r, "Connection", "close");
    json_response(r, j, status);
    /* Rejected requests may have unread bodies. Do not reuse their connection. */
    return ESP_FAIL;
}
static esp_err_t outcome(httpd_req_t *r, esp_err_t err)
{
    if (err != ESP_OK) {
        const char *message = "Operation failed. Inspect update state and diagnostics.";
        switch (err) {
        case ESP_ERR_INVALID_STATE:
            message = "Device is busy, not yet healthy, or cannot perform this operation in its "
                      "current state.";
            break;
        case ESP_ERR_INVALID_VERSION:
            message = "The candidate must have a strictly newer compatible firmware version.";
            break;
        case ESP_ERR_INVALID_SIZE:
            message = "Image or request size is outside the allowed bounds.";
            break;
        case ESP_ERR_INVALID_ARG:
            message = "Invalid request or incompatible image metadata.";
            break;
        case ESP_ERR_INVALID_CRC:
            message = "Image digest or signed data verification failed.";
            break;
        case ESP_ERR_OTA_VALIDATE_FAILED:
            message = "Application image or RSA signature verification failed.";
            break;
        case ESP_ERR_NO_MEM:
            message = "Insufficient memory; retry after other connections close.";
            break;
        default:
            break;
        }
        return error_response(r, "409 Conflict", esp_err_to_name(err), message);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return json_response(r, j, "200 OK");
}
static bool header(httpd_req_t *r, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    return httpd_req_get_hdr_value_str(r, key, out, cap) == ESP_OK;
}
static bool same_origin(httpd_req_t *r)
{
    char origin[160], host[128], expected[160];
    if (!httpd_req_get_hdr_value_len(r, "Origin"))
        return true;
    if (!header(r, "Origin", origin, sizeof(origin)) || !header(r, "Host", host, sizeof(host)))
        return false;
    snprintf(expected, sizeof(expected), "https://%s", host);
    return !strcmp(origin, expected);
}
static int session_index(httpd_req_t *r, bool mutation)
{
    char auth[96], cookie[256], token[65] = {0}, csrf[40];
    bool bearer = false;
    if (header(r, "Authorization", auth, sizeof(auth)) && !strncmp(auth, "Bearer ", 7) &&
        strlen(auth + 7) == 64) {
        memcpy(token, auth + 7, 64);
        bearer = true;
    } else if (header(r, "Cookie", cookie, sizeof(cookie))) {
        const char *p = cookie;
        while (p && *p) {
            while (*p == ' ' || *p == ';')
                p++;
            if (!strncmp(p, "sid=", 4) && strlen(p + 4) >= 64 && (!p[68] || p[68] == ';')) {
                memcpy(token, p + 4, 64);
                break;
            }
            p = strchr(p, ';');
            if (p)
                p++;
        }
    }
    if (!*token || !same_origin(r))
        return -1;
    header(r, "X-CSRF-Token", csrf, sizeof(csrf));
    return auth_validate(&auth_policy, esp_timer_get_time(), token, csrf, mutation && !bearer);
}

static bool read_body(httpd_req_t *r, void *buffer, size_t max)
{
    if (r->content_len <= 0 || r->content_len > max)
        return false;
    size_t at = 0;
    int64_t deadline = esp_timer_get_time() + 10000000;
    while (at < r->content_len && esp_timer_get_time() < deadline) {
        int n = httpd_req_recv(r, (char *)buffer + at, r->content_len - at);
        if (n <= 0)
            return false;
        at += (size_t)n;
    }
    return at == r->content_len;
}
static cJSON *read_json(httpd_req_t *r)
{
    char content[64];
    if (!header(r, "Content-Type", content, sizeof(content)) ||
        strcmp(content, "application/json") || !r->content_len || r->content_len > 2048)
        return NULL;
    char *body = calloc(1, r->content_len + 1);
    if (!body)
        return NULL;
    cJSON *j = read_body(r, body, 2048) ? protocol_object(body, r->content_len) : NULL;
    memset(body, 0, r->content_len);
    free(body);
    if (j && !cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}
static const char *string_value(cJSON *j, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool update_ticket(httpd_req_t *r, char out[65])
{
    return header(r, "X-Update-Ticket", out, 65) && ota_ticket_valid(out);
}

static esp_err_t route(httpd_req_t *r)
{
    const char *uri = r->uri;
    if (r->method == HTTP_GET &&
        (!strcmp(uri, "/") || !strcmp(uri, "/app.js") || !strcmp(uri, "/style.css"))) {
        const uint8_t *start = html_start, *end = html_end;
        const char *type = "text/html; charset=utf-8";
        if (!strcmp(uri, "/app.js")) {
            start = js_start;
            end = js_end;
            type = "text/javascript; charset=utf-8";
        }
        if (!strcmp(uri, "/style.css")) {
            start = css_start;
            end = css_end;
            type = "text/css; charset=utf-8";
        }
        headers(r);
        httpd_resp_set_type(r, type);
        return httpd_resp_send(r, (const char *)start, end - start);
    }
    if (!strcmp(uri, "/api/v1/session") && r->method == HTTP_POST) {
        if (!same_origin(r))
            return error_response(r, "403 Forbidden", "origin", "Origin mismatch");
        int64_t now = esp_timer_get_time();
        if (auth_throttled(&auth_policy, now))
            return error_response(r, "429 Too Many Requests", "rate_limit",
                                  "Wait one minute before retrying login.");
        cJSON *j = read_json(r);
        const char *password = string_value(j, "password");
        bool ok = check_password(password);
        if (password)
            memset((char *)password, 0, strlen(password));
        cJSON_Delete(j);
        if (!ok) {
            auth_failed(&auth_policy);
            return error_response(r, "401 Unauthorized", "authentication",
                                  "Administrator password was not accepted.");
        }
        char token[65], csrf[33];
        random_hex(token, 32);
        random_hex(csrf, 16);
        int index = auth_issue(&auth_policy, now, token, csrf);
        char cookie[160];
        snprintf(cookie, sizeof(cookie),
                 "sid=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=3600",
                 auth_policy.sessions[index].token);
        httpd_resp_set_hdr(r, "Set-Cookie", cookie);
        j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "token", auth_policy.sessions[index].token);
        cJSON_AddStringToObject(j, "csrf", auth_policy.sessions[index].csrf);
        return json_response(r, j, "200 OK");
    }
    char ticket[65];
    if (!strcmp(uri, "/api/v1/ota/confirm") && r->method == HTTP_POST) {
        if (!same_origin(r) || !header(r, "X-Update-Ticket", ticket, sizeof(ticket)))
            return error_response(r, "403 Forbidden", "ticket",
                                  "Update confirmation ticket required.");
        return outcome(r, ota_confirm(ticket));
    }
    if (!strcmp(uri, "/api/v1/ota/status") && r->method == HTTP_GET && update_ticket(r, ticket))
        return json_response(r, ota_status(), "200 OK");
    int session = session_index(r, r->method != HTTP_GET);
    if (session < 0)
        return error_response(r, "401 Unauthorized", "authentication",
                              "Sign in again. Sessions expire after one hour or reboot.");

    if (!strcmp(uri, "/api/v1/session") && r->method == HTTP_DELETE) {
        auth_revoke(&auth_policy, session);
        httpd_resp_set_hdr(r, "Set-Cookie",
                           "sid=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
        return outcome(r, ESP_OK);
    }
    if (!strcmp(uri, "/api/v1/status") && r->method == HTTP_GET)
        return json_response(r, diag_status(), "200 OK");
    if (!strcmp(uri, "/api/v1/diagnostics") && r->method == HTTP_GET) {
        cJSON *j = diag_status();
        cJSON_AddItemToObject(j, "logs", diag_logs(0));
        cJSON_AddItemToObject(j, "release", release_status());
        httpd_resp_set_hdr(r, "Content-Disposition",
                           "attachment; filename=entrance-diagnostics.json");
        return json_response(r, j, "200 OK");
    }
    if (!strcmp(uri, "/api/v1/logs") && r->method == HTTP_GET)
        return json_response(r, diag_logs(0), "200 OK");
    if (!strcmp(uri, "/api/v1/network") && r->method == HTTP_GET)
        return json_response(r, net_status(), "200 OK");
    if (!strcmp(uri, "/api/v1/network") && r->method == HTTP_PUT) {
        cJSON *j = read_json(r);
        const char *ssid = string_value(j, "ssid"), *password = string_value(j, "password");
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(j, "enabled");
        esp_err_t err = ESP_ERR_INVALID_ARG;
        xSemaphoreTake(operation_mutex, portMAX_DELAY);
        if (ota_busy())
            err = ESP_ERR_INVALID_STATE;
        else if (ssid && password && cJSON_IsBool(enabled)) {
            err = store_settings(ssid, password, cJSON_IsTrue(enabled));
            if (err == ESP_OK)
                err = net_apply();
        }
        xSemaphoreGive(operation_mutex);
        if (password)
            memset((char *)password, 0, strlen(password));
        cJSON_Delete(j);
        return outcome(r, err);
    }
    if (!strcmp(uri, "/api/v1/ota/status") && r->method == HTTP_GET)
        return json_response(r, ota_status(), "200 OK");
    if (!strcmp(uri, "/api/v1/ota/start") && r->method == HTTP_POST) {
        cJSON *j = read_json(r), *size = cJSON_GetObjectItemCaseSensitive(j, "size");
        const char *hash = string_value(j, "sha256");
        uint8_t bytes[32];
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (cJSON_IsNumber(size) && isfinite(size->valuedouble) && size->valuedouble > 0 &&
            size->valuedouble <= ENTRANCE_SLOT_BYTES &&
            floor(size->valuedouble) == size->valuedouble && hex_decode(hash, bytes, 32))
            err = ota_begin((size_t)size->valuedouble, bytes, NULL, ticket);
        cJSON_Delete(j);
        if (err != ESP_OK)
            return outcome(r, err);
        j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "ticket", ticket);
        cJSON_AddNumberToObject(j, "chunk_size", ENTRANCE_CHUNK);
        return json_response(r, j, "201 Created");
    }
    if (!strcmp(uri, "/api/v1/ota/chunk") && r->method == HTTP_PUT) {
        char offset_text[24], *end = NULL;
        if (!update_ticket(r, ticket) ||
            !header(r, "X-Upload-Offset", offset_text, sizeof(offset_text)))
            return error_response(r, "403 Forbidden", "ticket",
                                  "Valid ticket and upload offset required.");
        errno = 0;
        unsigned long at = strtoul(offset_text, &end, 10);
        if (errno || !*offset_text || !end || *end || at > ENTRANCE_SLOT_BYTES ||
            r->content_len > ENTRANCE_CHUNK)
            return outcome(r, ESP_ERR_INVALID_SIZE);
        uint8_t *buffer = malloc(ENTRANCE_CHUNK);
        if (!buffer)
            return outcome(r, ESP_ERR_NO_MEM);
        esp_err_t err = read_body(r, buffer, ENTRANCE_CHUNK) ? ota_write(at, buffer, r->content_len)
                                                             : ESP_ERR_TIMEOUT;
        free(buffer);
        return outcome(r, err);
    }
    if (!strcmp(uri, "/api/v1/ota/finish") && r->method == HTTP_POST) {
        if (!update_ticket(r, ticket))
            return error_response(r, "403 Forbidden", "ticket", "Valid update ticket required.");
        return outcome(r, ota_finish());
    }
    if (!strcmp(uri, "/api/v1/ota/abort") && r->method == HTTP_POST) {
        ota_abort("Update cancelled by administrator");
        return outcome(r, ESP_OK);
    }
    if (!strcmp(uri, "/api/v1/ota/rollback") && r->method == HTTP_POST)
        return outcome(r, ota_rollback());
    if (!strcmp(uri, "/api/v1/releases") && r->method == HTTP_GET)
        return json_response(r, release_status(), "200 OK");
    if (!strcmp(uri, "/api/v1/releases/check") && r->method == HTTP_POST)
        return outcome(r, release_check());
    if (!strcmp(uri, "/api/v1/releases/install") && r->method == HTTP_POST) {
        esp_err_t err = release_install(ticket);
        if (err != ESP_OK)
            return outcome(r, err);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "ticket", ticket);
        return json_response(r, j, "202 Accepted");
    }
    return error_response(r, "404 Not Found", "not_found", "Unknown operation or HTTP method.");
}

esp_err_t api_start(void)
{
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;
    cfg.httpd.max_uri_handlers = 8;
    cfg.httpd.max_open_sockets = 3;
    cfg.httpd.stack_size = 12288;
    cfg.httpd.recv_wait_timeout = 5;
    cfg.httpd.send_wait_timeout = 5;
    cfg.httpd.lru_purge_enable = true;
    cfg.servercert = (const uint8_t *)identity.certificate;
    cfg.servercert_len = strlen(identity.certificate) + 1;
    cfg.prvtkey_pem = (const uint8_t *)identity.private_key;
    cfg.prvtkey_len = strlen(identity.private_key) + 1;
    esp_err_t err = httpd_ssl_start(&server, &cfg);
    if (err != ESP_OK)
        return err;
    httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE};
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        httpd_uri_t uri = {.uri = "/*", .method = methods[i], .handler = route};
        if ((err = httpd_register_uri_handler(server, &uri)) != ESP_OK) {
            httpd_ssl_stop(server);
            server = NULL;
            return err;
        }
    }
    return ESP_OK;
}
bool api_ready(void)
{
    return server != NULL;
}
