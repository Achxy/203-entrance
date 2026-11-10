#include "entrance.h"
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

identity_t identity;
settings_t settings;
_Atomic bool settings_healthy = true;
SemaphoreHandle_t operation_mutex;
static SemaphoreHandle_t storage_mutex;
static nvs_handle_t config_handle, update_handle;

void sha256_bytes(const void *data, size_t len, uint8_t out[32])
{
    size_t written = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &written) != PSA_SUCCESS ||
        written != 32)
        abort();
}

void random_hex(char *out, size_t bytes)
{
    uint8_t buffer[32];
    if (bytes > sizeof(buffer))
        abort();
    esp_fill_random(buffer, bytes);
    hex_encode(buffer, bytes, out);
    memset(buffer, 0, sizeof(buffer));
}

static esp_err_t read_string(nvs_handle_t h, const char *key, char *dst, size_t capacity)
{
    size_t n = capacity;
    return nvs_get_str(h, key, dst, &n);
}

static esp_err_t read_alloc(nvs_handle_t h, const char *key, char **dst)
{
    size_t size = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &size);
    if (err != ESP_OK || size < 2 || size > 8192)
        return ESP_ERR_INVALID_SIZE;
    *dst = calloc(1, size);
    if (!*dst)
        return ESP_ERR_NO_MEM;
    return nvs_get_str(h, key, *dst, &size);
}

settings_t settings_snapshot(void)
{
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    settings_t snapshot = settings;
    xSemaphoreGive(storage_mutex);
    return snapshot;
}

esp_err_t store_init(void)
{
    if (psa_crypto_init() != PSA_SUCCESS)
        return ESP_FAIL;
    operation_mutex = xSemaphoreCreateMutex();
    storage_mutex = xSemaphoreCreateMutex();
    if (!operation_mutex || !storage_mutex)
        return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_flash_init_partition("identity");
    if (err != ESP_OK)
        return err;
    nvs_handle_t h;
    if ((err = nvs_open_from_partition("identity", "device", NVS_READONLY, &h)) != ESP_OK)
        return err;
    char salt[33] = {0}, hash[65] = {0};
    err = read_string(h, "ssid", identity.ssid, sizeof(identity.ssid));
    if (err == ESP_OK)
        err = read_string(h, "ap_pass", identity.ap_password, sizeof(identity.ap_password));
    if (err == ESP_OK)
        err = read_string(h, "id", identity.device_id, sizeof(identity.device_id));
    if (err == ESP_OK)
        err = read_string(h, "salt", salt, sizeof(salt));
    if (err == ESP_OK)
        err = read_string(h, "admin_hash", hash, sizeof(hash));
    if (err == ESP_OK)
        err = read_alloc(h, "cert", &identity.certificate);
    if (err == ESP_OK)
        err = read_alloc(h, "tls_key", &identity.private_key);
    if (err == ESP_OK)
        err = read_alloc(h, "sign_pub", &identity.signing_public);
    nvs_close(h);
    if (err != ESP_OK || !hex_decode(salt, identity.admin_salt, 16) ||
        !hex_decode(hash, identity.admin_hash, 32) || strlen(identity.ap_password) < 12)
        return ESP_ERR_INVALID_STATE;

    settings.schema = ENTRANCE_SCHEMA;
    err = nvs_flash_init();
    if (err == ESP_OK)
        err = nvs_open("cfg_v1", NVS_READWRITE, &config_handle);
    if (err != ESP_OK) {
        settings_healthy = false;
        ESP_LOGE("store", "Settings storage requires recovery: %s", esp_err_to_name(err));
    } else {
        settings_t a = {0}, b = {0};
        size_t na = sizeof(a), nb = sizeof(b);
        esp_err_t ea = nvs_get_blob(config_handle, "a", &a, &na),
                  eb = nvs_get_blob(config_handle, "b", &b, &nb);
        bool selected = settings_select(&a, ea == ESP_OK && na == sizeof(a), &b,
                                        eb == ESP_OK && nb == sizeof(b), &settings);
        if (!selected && (ea != ESP_ERR_NVS_NOT_FOUND || eb != ESP_ERR_NVS_NOT_FOUND))
            settings_healthy = false;
    }
    err = nvs_flash_init_partition("state");
    if (err == ESP_OK)
        err = nvs_open_from_partition("state", "ota_v1", NVS_READWRITE, &update_handle);
    if (err != ESP_OK) {
        settings_healthy = false;
        ESP_LOGE("store", "Update journal unavailable: %s", esp_err_to_name(err));
    }
    return ESP_OK; /* Identity still permits local diagnostics and USB recovery. */
}

esp_err_t store_settings(const char *ssid, const char *password, bool enabled)
{
    if (!settings_healthy || !config_handle)
        return ESP_ERR_INVALID_STATE;
    if (!ssid || !password || strlen(ssid) > 32 || strlen(password) > 63 ||
        (enabled && (!*ssid || (*password && strlen(password) < 8))))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    settings_t next = {0};
    next.schema = ENTRANCE_SCHEMA;
    next.generation = settings.generation + 1;
    if (!next.generation) {
        xSemaphoreGive(storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(next.ssid, ssid, sizeof(next.ssid));
    strlcpy(next.password, password, sizeof(next.password));
    next.enabled = enabled;
    next.crc = record_crc(&next, offsetof(settings_t, crc));
    esp_err_t err =
        nvs_set_blob(config_handle, next.generation & 1 ? "a" : "b", &next, sizeof(next));
    if (err == ESP_OK)
        err = nvs_commit(config_handle);
    if (err == ESP_OK)
        settings = next;
    else
        settings_healthy = false;
    xSemaphoreGive(storage_mutex);
    return err;
}

esp_err_t store_update(update_record_t *r)
{
    if (!update_handle)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    r->magic = ENTRANCE_MAGIC;
    r->crc = record_crc(r, offsetof(update_record_t, crc));
    esp_err_t err = nvs_set_blob(update_handle, "transaction", r, sizeof(*r));
    if (err == ESP_OK)
        err = nvs_commit(update_handle);
    if (err != ESP_OK)
        settings_healthy = false;
    xSemaphoreGive(storage_mutex);
    return err;
}

esp_err_t load_update(update_record_t *r)
{
    if (!update_handle)
        return ESP_ERR_INVALID_STATE;
    size_t size = sizeof(*r);
    esp_err_t err = nvs_get_blob(update_handle, "transaction", r, &size);
    if (err != ESP_OK)
        return err;
    if (size != sizeof(*r) || r->magic != ENTRANCE_MAGIC ||
        r->crc != record_crc(r, offsetof(update_record_t, crc)) ||
        !memchr(r->version, 0, sizeof(r->version)) || !memchr(r->result, 0, sizeof(r->result)) ||
        !memchr(r->detail, 0, sizeof(r->detail)))
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

bool check_password(const char *password)
{
    if (!password || strlen(password) < 12 || strlen(password) > 128)
        return false;
    uint8_t hash[32] = {0};
    psa_key_derivation_operation_t ctx = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t err = psa_key_derivation_setup(&ctx, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (err == PSA_SUCCESS)
        err = psa_key_derivation_input_integer(&ctx, PSA_KEY_DERIVATION_INPUT_COST, 100000);
    if (err == PSA_SUCCESS)
        err = psa_key_derivation_input_bytes(&ctx, PSA_KEY_DERIVATION_INPUT_SALT,
                                             identity.admin_salt, 16);
    if (err == PSA_SUCCESS)
        err = psa_key_derivation_input_bytes(&ctx, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                             (const uint8_t *)password, strlen(password));
    if (err == PSA_SUCCESS)
        err = psa_key_derivation_output_bytes(&ctx, hash, 32);
    psa_key_derivation_abort(&ctx);
    bool ok = !err && constant_equal(hash, identity.admin_hash, 32);
    memset(hash, 0, sizeof(hash));
    return ok;
}
