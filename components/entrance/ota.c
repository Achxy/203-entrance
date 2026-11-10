#include "entrance.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <string.h>

static esp_ota_handle_t writer;
static const esp_partition_t *destination;
static psa_hash_operation_t digest = PSA_HASH_OPERATION_INIT;
static update_record_t record;
static size_t received;
static atomic_bool active, pending, reboot_scheduled;
static bool acknowledged, header_checked, baseline_healthy;
static int64_t last_activity, started, reboot_at, healthy_since, acknowledged_at;
static char expected_version[32];
static char phase[24] = "idle", detail[96] = "Ready";
static uint8_t prefix[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                      sizeof(esp_app_desc_t) + sizeof(entrance_descriptor_t)];
static size_t prefix_used;

static void state(const char *next, const char *message)
{
    strlcpy(phase, next, sizeof(phase));
    strlcpy(detail, message, sizeof(detail));
    diag_event(message);
}

static void abort_locked(const char *message)
{
    if (writer)
        esp_ota_abort(writer);
    writer = 0;
    psa_hash_abort(&digest);
    active = false;
    state("failed", message);
}

static bool running_matches(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (record.image_size < sizeof(prefix) || record.image_size > running->size)
        return false;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS)
        return false;
    uint8_t buf[1024], hash[32];
    size_t count = 0;
    bool ok = true;
    for (size_t at = 0; at < record.image_size; at += sizeof(buf)) {
        size_t n = record.image_size - at;
        if (n > sizeof(buf))
            n = sizeof(buf);
        if (esp_partition_read(running, at, buf, n) != ESP_OK ||
            psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = psa_hash_finish(&op, hash, sizeof(hash), &count) == PSA_SUCCESS && count == 32 &&
             constant_equal(hash, record.image_hash, 32);
    psa_hash_abort(&op);
    return ok;
}

void ota_init(void)
{
    esp_ota_img_states_t boot_state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(esp_ota_get_running_partition(), &boot_state);
    pending = boot_state == ESP_OTA_IMG_PENDING_VERIFY;
    esp_err_t err = load_update(&record);
    if (err != ESP_OK)
        memset(&record, 0, sizeof(record));
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        settings_healthy = false;
    if (pending) {
        if (err == ESP_OK && running_matches())
            state("trial", "Trial boot: waiting for healthy services and updater reconnection");
        else {
            settings_healthy = false;
            state("trial", "Trial journal mismatch; previous firmware will be restored");
        }
    } else if (record.magic == ENTRANCE_MAGIC &&
               (!strcmp(record.result, "armed") || !strcmp(record.result, "confirmed") ||
                !strcmp(record.result, "rollback_requested"))) {
        /* Boot metadata is authoritative if power failed after marking VALID
           but before the second journal commit. Never misreport that as rollback. */
        bool confirmed = boot_state == ESP_OTA_IMG_VALID && running_matches();
        strlcpy(record.result, confirmed ? "confirmed" : "rolled_back", sizeof(record.result));
        strlcpy(record.detail,
                confirmed ? "Firmware confirmed healthy" : "Previous firmware restored",
                sizeof(record.detail));
        if (store_update(&record) != ESP_OK)
            settings_healthy = false;
        state(record.result, record.detail);
    } else if (!strcmp(record.result, "rolled_back"))
        state("rolled_back", record.detail);
}

bool ota_busy(void)
{
    return active || pending || reboot_scheduled;
}
bool ota_trial(void)
{
    return pending;
}

esp_err_t ota_begin(size_t size, const uint8_t hash[32], const char *version, char ticket[65])
{
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (ota_busy() || !settings_healthy || !baseline_healthy)
        err = ESP_ERR_INVALID_STATE;
    else if (size < sizeof(prefix) + 4096 || size > ENTRANCE_SLOT_BYTES || size % 4096)
        err = ESP_ERR_INVALID_SIZE;
    else {
        destination = esp_ota_get_next_update_partition(NULL);
        if (!destination || destination == esp_ota_get_running_partition() ||
            size > destination->size)
            err = ESP_ERR_NOT_FOUND;
        else {
            memset(&record, 0, sizeof(record));
            record.magic = ENTRANCE_MAGIC;
            record.image_size = size;
            memcpy(record.image_hash, hash, 32);
            random_hex(ticket, 32);
            sha256_bytes(ticket, 64, record.ticket_hash);
            received = prefix_used = 0;
            header_checked = false;
            acknowledged = false;
            acknowledged_at = 0;
            strlcpy(expected_version, version ? version : "", sizeof(expected_version));
            digest = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
            if (psa_hash_setup(&digest, PSA_ALG_SHA_256) != PSA_SUCCESS)
                err = ESP_FAIL;
            else
                err = esp_ota_begin(destination, size, &writer);
            if (err == ESP_OK) {
                active = true;
                started = last_activity = esp_timer_get_time();
                state(version ? "downloading" : "receiving",
                      version ? "Downloading signed release into inactive slot"
                              : "Receiving signed image into inactive slot");
            } else {
                psa_hash_abort(&digest);
                writer = 0;
            }
        }
    }
    xSemaphoreGive(operation_mutex);
    return err;
}

static esp_err_t check_header(void)
{
    esp_image_header_t image;
    esp_app_desc_t app;
    entrance_descriptor_t custom;
    memcpy(&image, prefix, sizeof(image));
    memcpy(&app, prefix + sizeof(image) + sizeof(esp_image_segment_header_t), sizeof(app));
    memcpy(&custom, prefix + sizeof(image) + sizeof(esp_image_segment_header_t) + sizeof(app),
           sizeof(custom));
    if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != ESP_CHIP_ID_ESP32C3 ||
        app.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !memchr(app.project_name, 0, sizeof(app.project_name)) ||
        !memchr(app.version, 0, sizeof(app.version)) ||
        strcmp(app.project_name, ENTRANCE_PROJECT) || !descriptor_compatible(&custom) ||
        (*expected_version && strcmp(expected_version, app.version)))
        return ESP_ERR_INVALID_ARG;
    bool valid = false;
    if (version_compare(app.version, esp_app_get_description()->version, &valid) <= 0 || !valid)
        return ESP_ERR_INVALID_VERSION;
    esp_err_t err = esp_ota_check_image_validity(ESP_PARTITION_TYPE_APP, &image, &app);
    if (err == ESP_OK)
        strlcpy(record.version, app.version, sizeof(record.version));
    return err;
}

esp_err_t ota_write(size_t offset, const void *data, size_t len)
{
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!active || offset != received)
        err = ESP_ERR_INVALID_STATE;
    else if (!len || len > ENTRANCE_CHUNK || len > record.image_size - received)
        err = ESP_ERR_INVALID_SIZE;
    else {
        if (prefix_used < sizeof(prefix)) {
            size_t n = sizeof(prefix) - prefix_used;
            if (n > len)
                n = len;
            memcpy(prefix + prefix_used, data, n);
            prefix_used += n;
            if (prefix_used == sizeof(prefix)) {
                err = check_header();
                header_checked = err == ESP_OK;
            }
        }
        if (err == ESP_OK)
            err = esp_ota_write(writer, data, len);
        if (err == ESP_OK && psa_hash_update(&digest, data, len) != PSA_SUCCESS)
            err = ESP_FAIL;
        if (err == ESP_OK) {
            received += len;
            last_activity = esp_timer_get_time();
        } else
            abort_locked("Image rejected: incompatible header, version, or flash write failure");
    }
    xSemaphoreGive(operation_mutex);
    return err;
}

esp_err_t ota_finish(void)
{
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    uint8_t hash[32];
    size_t len = 0;
    if (!active || received != record.image_size || !header_checked)
        err = ESP_ERR_INVALID_STATE;
    else {
        state("verifying", "Verifying complete image hash and RSA signature");
        if (psa_hash_finish(&digest, hash, sizeof(hash), &len) != PSA_SUCCESS || len != 32 ||
            !constant_equal(hash, record.image_hash, 32))
            err = ESP_ERR_INVALID_CRC;
        if (err == ESP_OK) {
            err = esp_ota_end(writer);
            writer = 0;
        }
        if (err == ESP_OK) {
            strlcpy(record.result, "armed", sizeof(record.result));
            strlcpy(record.detail, "Verified image selected for trial boot", sizeof(record.detail));
            err = store_update(&record); /* Durable ticket must precede boot selection. */
        }
        if (err == ESP_OK)
            err = esp_ota_set_boot_partition(destination);
        if (err == ESP_OK) {
            active = false;
            state("rebooting", "Verified image ready; reconnecting after reboot");
            ota_schedule_reboot();
        } else
            abort_locked("Verification failed; current firmware remains active");
    }
    xSemaphoreGive(operation_mutex);
    return err;
}

void ota_abort(const char *message)
{
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    if (active)
        abort_locked(message);
    xSemaphoreGive(operation_mutex);
}

static bool ticket_matches(const char *ticket)
{
    if (!ticket || strlen(ticket) != 64 || record.magic != ENTRANCE_MAGIC)
        return false;
    uint8_t hash[32];
    sha256_bytes(ticket, 64, hash);
    return constant_equal(hash, record.ticket_hash, 32);
}

bool ota_ticket_valid(const char *ticket)
{
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    bool valid = ticket_matches(ticket);
    xSemaphoreGive(operation_mutex);
    return valid;
}

esp_err_t ota_confirm(const char *ticket)
{
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (ticket_matches(ticket) && pending &&
        esp_timer_get_time() < ENTRANCE_TRIAL_SECONDS * 1000000LL) {
        acknowledged = true;
        acknowledged_at = esp_timer_get_time();
        err = ESP_OK;
    } else if (ticket_matches(ticket) && !pending && !strcmp(record.result, "confirmed"))
        err = ESP_OK;
    xSemaphoreGive(operation_mutex);
    return err;
}

void ota_schedule_reboot(void)
{
#if ENTRANCE_TEST_FAULT == 4
    reboot_at = esp_timer_get_time() + 60000000;
#else
    reboot_at = esp_timer_get_time() + 2000000;
#endif
    reboot_scheduled = true;
}

void ota_tick(bool healthy)
{
    if (xSemaphoreTake(operation_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return;
    int64_t now = esp_timer_get_time();
    if (!healthy)
        healthy_since = 0;
    else if (!healthy_since)
        healthy_since = now;
    bool stable = healthy_since && now - healthy_since >= ENTRANCE_HEALTH_SECONDS * 1000000LL;
    baseline_healthy = stable;
    if (reboot_at && now >= reboot_at) {
        xSemaphoreGive(operation_mutex);
        esp_restart();
        return;
    }
    if (active && (now - last_activity > 30000000 || now - started > 300000000))
        abort_locked("Transfer timed out; retry the update from the beginning");
    if (pending && stable && acknowledged && now - acknowledged_at <= 10000000 &&
        now < ENTRANCE_TRIAL_SECONDS * 1000000LL) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            pending = false;
            strlcpy(record.result, "confirmed", sizeof(record.result));
            strlcpy(record.detail, "Health checks and updater reconnection passed",
                    sizeof(record.detail));
            if (store_update(&record) != ESP_OK) {
                settings_healthy = false;
                state("confirmed",
                      "Healthy firmware confirmed; journal write failed, maintenance required");
            } else
                state("confirmed", record.detail);
        }
    }
    if (pending && now >= ENTRANCE_TRIAL_SECONDS * 1000000LL) {
        state("rolling_back", "Trial did not confirm; restoring previous firmware");
        xSemaphoreGive(operation_mutex);
        esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        if (err != ESP_OK) {
            diag_event("Rollback unavailable; USB recovery required");
            net_maintenance();
        }
        return;
    }
    if (!pending && stable) {
        esp_ota_img_states_t s;
        if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) != ESP_OK ||
            s == ESP_OTA_IMG_UNDEFINED)
            esp_ota_mark_app_valid_cancel_rollback();
    }
    xSemaphoreGive(operation_mutex);
}

esp_err_t ota_rollback(void)
{
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!ota_busy() && esp_ota_check_rollback_is_possible()) {
        strlcpy(record.result, "rollback_requested", sizeof(record.result));
        err = store_update(&record);
        if (err == ESP_OK)
            err = esp_ota_mark_app_invalid_rollback();
        if (err == ESP_OK) {
            state("rolling_back", "Manual rollback selected");
            ota_schedule_reboot();
        }
    }
    xSemaphoreGive(operation_mutex);
    return err;
}

cJSON *ota_status(void)
{
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "phase", phase);
    cJSON_AddStringToObject(j, "detail", detail);
    cJSON_AddStringToObject(j, "target_version", record.version);
    cJSON_AddStringToObject(j, "running_version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(j, "received", received);
    cJSON_AddNumberToObject(j, "total", record.image_size);
    cJSON_AddBoolToObject(j, "busy", ota_busy());
    cJSON_AddBoolToObject(j, "trial", pending);
    cJSON_AddBoolToObject(j, "reconnected",
                          acknowledged &&
                              (!pending || esp_timer_get_time() - acknowledged_at <= 10000000));
    cJSON_AddBoolToObject(j, "healthy", baseline_healthy);
    cJSON_AddNumberToObject(j, "uptime_ms", esp_timer_get_time() / 1000);
    xSemaphoreGive(operation_mutex);
    return j;
}
