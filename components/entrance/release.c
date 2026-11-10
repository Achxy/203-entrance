#include "entrance.h"
#include "protocol.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define RELEASE_BASE "https://github.com/Achxy/203-entrance-firmware/releases/"
typedef struct {
    char version[32], url[256];
    uint8_t hash[32];
    size_t size;
    bool valid;
} release_t;
static release_t available;
static _Atomic(SemaphoreHandle_t) release_mutex;
static bool checking, check_requested, install_requested, installing;
static char release_message[128] = "Offline: connect an optional Wi-Fi uplink to check GitHub";
static int64_t next_check;
static char requested_version[32];
typedef struct {
    char location[2048];
} redirect_t;

static bool allowed_url(const char *url)
{
    const char *prefixes[] = {RELEASE_BASE, "https://release-assets.githubusercontent.com/",
                              "https://objects.githubusercontent.com/"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strncmp(url, prefixes[i], strlen(prefixes[i])))
            return true;
    return false;
}
static esp_err_t http_event(esp_http_client_event_t *e)
{
    redirect_t *r = e->user_data;
    if (r && e->event_id == HTTP_EVENT_ON_HEADER && e->header_key &&
        !strcasecmp(e->header_key, "Location")) {
        if (strlen(e->header_value) >= sizeof(r->location))
            return ESP_ERR_INVALID_SIZE;
        strlcpy(r->location, e->header_value, sizeof(r->location));
    }
    return ESP_OK;
}
static esp_http_client_handle_t open_download(const char *initial, int64_t *length)
{
    if (!net_online() || time(NULL) < 1700000000)
        return NULL;
    char *url = strdup(initial);
    redirect_t *redirect = calloc(1, sizeof(*redirect));
    if (!url || !redirect) {
        free(url);
        free(redirect);
        return NULL;
    }
    esp_http_client_handle_t client = NULL;
    for (int hop = 0; hop < 6; hop++) {
        if (!allowed_url(url))
            break;
        redirect->location[0] = 0;
        esp_http_client_config_t cfg = {.url = url,
                                        .crt_bundle_attach = esp_crt_bundle_attach,
                                        .timeout_ms = 10000,
                                        .disable_auto_redirect = true,
                                        .buffer_size = 4096,
                                        .buffer_size_tx = 1024,
                                        .event_handler = http_event,
                                        .user_data = redirect,
                                        .user_agent = "203-entrance/1"};
        client = esp_http_client_init(&cfg);
        if (!client)
            break;
        if (esp_http_client_open(client, 0) != ESP_OK) {
            esp_http_client_cleanup(client);
            client = NULL;
            break;
        }
        *length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200 && *length > 0 && !esp_http_client_is_chunked_response(client))
            break;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
        if (status < 300 || status > 399 || !redirect->location[0])
            break;
        free(url);
        url = strdup(redirect->location);
        if (!url)
            break;
    }
    /* The callback data must remain valid until cleanup, so detach it after headers. */
    if (client)
        esp_http_client_set_user_data(client, NULL);
    free(url);
    free(redirect);
    return client;
}
static esp_err_t small_download(const char *url, uint8_t *out, size_t capacity, size_t *used)
{
    int64_t total = 0;
    esp_http_client_handle_t client = open_download(url, &total);
    if (!client)
        return ESP_FAIL;
    esp_err_t err = ESP_OK;
    size_t at = 0;
    if (total > (int64_t)capacity)
        err = ESP_ERR_INVALID_SIZE;
    while (err == ESP_OK && at < (size_t)total) {
        int n = esp_http_client_read(client, (char *)out + at, (size_t)total - at);
        if (n <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        at += n;
    }
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(client))
        err = ESP_ERR_INVALID_SIZE;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    *used = at;
    return err;
}
static bool verify_manifest(const uint8_t *bytes, size_t len, const uint8_t *sig, size_t siglen)
{
    if (siglen != 384 || strlen(identity.signing_public) % 2)
        return false;
    size_t derlen = strlen(identity.signing_public) / 2;
    if (derlen < 256 || derlen > 1024)
        return false;
    uint8_t der[1024], hash[32];
    if (!hex_decode(identity.signing_public, der, derlen))
        return false;
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
    psa_set_key_bits(&attrs, 3072);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attrs, PSA_ALG_RSA_PSS(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key = 0;
    psa_status_t result = psa_import_key(&attrs, der, derlen, &key);
    if (result == PSA_SUCCESS) {
        sha256_bytes(bytes, len, hash);
        result = psa_verify_hash(key, PSA_ALG_RSA_PSS(PSA_ALG_SHA_256), hash, 32, sig, siglen);
        psa_destroy_key(key);
    }
    psa_reset_key_attributes(&attrs);
    return result == PSA_SUCCESS;
}
static esp_err_t fetch_release(release_t *out, const char *version)
{
    uint8_t *body = calloc(1, 4097), sig[384];
    size_t len = 0, siglen = 0;
    if (!body)
        return ESP_ERR_NO_MEM;
    char base[192], url[224];
    if (*version)
        snprintf(base, sizeof(base), RELEASE_BASE "download/v%s/", version);
    else
        strlcpy(base, RELEASE_BASE "latest/download/", sizeof(base));
    snprintf(url, sizeof(url), "%smanifest.json", base);
    esp_err_t err = small_download(url, body, 4096, &len);
    snprintf(url, sizeof(url), "%smanifest.sig", base);
    if (err == ESP_OK)
        err = small_download(url, sig, sizeof(sig), &siglen);
    if (err == ESP_OK && !verify_manifest(body, len, sig, siglen))
        err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK) {
        release_manifest_t manifest;
        if (!protocol_manifest((char *)body, len, &manifest) ||
            (*version && strcmp(version, manifest.version)))
            err = ESP_ERR_INVALID_ARG;
        else {
            strlcpy(out->version, manifest.version, sizeof(out->version));
            memcpy(out->hash, manifest.hash, sizeof(out->hash));
            out->size = manifest.size;
            snprintf(out->url, sizeof(out->url), RELEASE_BASE "download/v%s/firmware.bin",
                     out->version);
            out->valid = true;
        }
    }
    free(body);
    return err;
}

static esp_err_t download_image(release_t *r)
{
    int64_t total = 0;
    esp_http_client_handle_t client = NULL;
    for (int attempt = 0; attempt < 3 && !client; attempt++) {
        client = open_download(r->url, &total);
        if (!client)
            vTaskDelay(pdMS_TO_TICKS((1u << attempt) * 1000));
    }
    if (!client)
        return ESP_FAIL;
    esp_err_t err = total == (int64_t)r->size ? ESP_OK : ESP_ERR_INVALID_SIZE;
    uint8_t *buffer = malloc(4096);
    size_t at = 0;
    if (!buffer)
        err = ESP_ERR_NO_MEM;
    while (err == ESP_OK && at < r->size) {
        size_t wanted = r->size - at;
        if (wanted > 4096)
            wanted = 4096;
        int n = esp_http_client_read(client, (char *)buffer, wanted);
        if (n <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        err = ota_write(at, buffer, n);
        at += n;
        vTaskDelay(1);
    }
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(client))
        err = ESP_ERR_INVALID_SIZE;
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK)
        err = ota_finish();
    return err;
}
static void worker(void *unused)
{
    (void)unused;
    next_check = esp_timer_get_time() + 30000000;
    for (;;) {
        xSemaphoreTake(release_mutex, portMAX_DELAY);
        bool install = install_requested;
        bool check =
            check_requested || (net_online() && esp_timer_get_time() >= next_check && !ota_busy());
        release_t release = available;
        char version[32];
        strlcpy(version, requested_version, sizeof(version));
        requested_version[0] = 0;
        install_requested = check_requested = false;
        if (check && !install)
            checking = true;
        xSemaphoreGive(release_mutex);
        if (install) {
            esp_err_t err = download_image(&release);
            if (err != ESP_OK)
                ota_abort("Remote transfer failed; current firmware retained. Retry when the "
                          "uplink is working.");
            xSemaphoreTake(release_mutex, portMAX_DELAY);
            installing = false;
            strlcpy(release_message,
                    err == ESP_OK ? "Signed release downloaded and selected for trial"
                                  : "Remote download failed; retry manually",
                    sizeof(release_message));
            xSemaphoreGive(release_mutex);
        } else if (check) {
            release_t candidate = {0};
            esp_err_t err = fetch_release(&candidate, version);
            xSemaphoreTake(release_mutex, portMAX_DELAY);
            checking = false;
            if (err == ESP_OK) {
                available = candidate;
                strlcpy(release_message, "Signed release manifest verified",
                        sizeof(release_message));
            } else {
                available.valid = false;
                strlcpy(release_message,
                        "Check failed: verify uplink, clock, published assets, and signing key",
                        sizeof(release_message));
            }
            next_check = esp_timer_get_time() + (err == ESP_OK ? 21600000000LL : 300000000LL) +
                         (esp_random() % 600) * 1000000LL;
            xSemaphoreGive(release_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void release_start(void)
{
    release_mutex = xSemaphoreCreateMutex();
    /* Share CPU with IDLE during TLS crypto; keep watchdog and health service live. */
    if (!release_mutex ||
        xTaskCreate(worker, "releases", 10240, NULL, tskIDLE_PRIORITY, NULL) != pdPASS)
        abort();
}
esp_err_t release_check(const char *version)
{
    if (version) {
        bool valid = false;
        version_compare(version, version, &valid);
        if (!valid || strchr(version, '-'))
            return ESP_ERR_INVALID_VERSION;
    }
    if (!release_mutex || !net_online() || ota_busy())
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(release_mutex, portMAX_DELAY);
    esp_err_t err = checking || check_requested || installing ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) {
        strlcpy(requested_version, version ? version : "", sizeof(requested_version));
        check_requested = true;
    }
    xSemaphoreGive(release_mutex);
    return err;
}
esp_err_t release_install(char ticket[65])
{
    if (!release_mutex || !net_online())
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(release_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (available.valid && !installing && !checking) {
        bool valid = false;
        int order = version_compare(available.version, esp_app_get_description()->version, &valid);
        err = valid && order > 0
                  ? ota_begin(available.size, available.hash, available.version, ticket)
                  : ESP_ERR_INVALID_VERSION;
        if (err == ESP_OK) {
            installing = true;
            install_requested = true;
        }
    }
    xSemaphoreGive(release_mutex);
    return err;
}
cJSON *release_status(void)
{
    cJSON *j = cJSON_CreateObject();
    if (!release_mutex)
        return j;
    xSemaphoreTake(release_mutex, portMAX_DELAY);
    cJSON_AddBoolToObject(j, "verified", available.valid);
    bool version_valid = false;
    int order =
        version_compare(available.version, esp_app_get_description()->version, &version_valid);
    cJSON_AddBoolToObject(j, "newer", available.valid && version_valid && order > 0);
    cJSON_AddBoolToObject(j, "checking", checking);
    cJSON_AddBoolToObject(j, "installing", installing);
    cJSON_AddStringToObject(j, "message", release_message);
    if (available.valid) {
        cJSON_AddStringToObject(j, "version", available.version);
        cJSON_AddNumberToObject(j, "size", available.size);
    }
    xSemaphoreGive(release_mutex);
    return j;
}
