#include "entrance.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include <stdio.h>
#include <string.h>

#define LOG_LINES 64
static struct {
    uint32_t id;
    int64_t ms;
    char text[128];
} lines[LOG_LINES];
static uint32_t sequence;
static portMUX_TYPE log_mux = portMUX_INITIALIZER_UNLOCKED;

void diag_init(void)
{
    diag_event("Boot started");
}
void diag_event(const char *message)
{
    portENTER_CRITICAL(&log_mux);
    uint32_t id = ++sequence;
    lines[id % LOG_LINES].id = id;
    lines[id % LOG_LINES].ms = esp_timer_get_time() / 1000;
    strlcpy(lines[id % LOG_LINES].text, message, sizeof(lines[0].text));
    portEXIT_CRITICAL(&log_mux);
}
cJSON *diag_logs(uint32_t after)
{
    cJSON *list = cJSON_CreateArray();
    portENTER_CRITICAL(&log_mux);
    uint32_t end = sequence;
    portEXIT_CRITICAL(&log_mux);
    uint32_t start = end >= LOG_LINES ? end - LOG_LINES + 1 : 1;
    if (after >= start && after < end)
        start = after + 1;
    if (after == end)
        return list;
    for (uint32_t n = 0; n < LOG_LINES && n <= end - start; n++) {
        uint32_t i = start + n;
        char text[128];
        int64_t ms;
        uint32_t id;
        portENTER_CRITICAL(&log_mux);
        id = lines[i % LOG_LINES].id;
        ms = lines[i % LOG_LINES].ms;
        strlcpy(text, lines[i % LOG_LINES].text, sizeof(text));
        portEXIT_CRITICAL(&log_mux);
        if (id != i)
            continue;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", id);
        cJSON_AddNumberToObject(j, "ms", ms);
        cJSON_AddStringToObject(j, "message", text);
        cJSON_AddItemToArray(list, j);
    }
    return list;
}
cJSON *diag_status(void)
{
    cJSON *j = cJSON_CreateObject();
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *p = esp_ota_get_running_partition();
    cJSON_AddStringToObject(j, "device_id", identity.device_id);
    cJSON_AddStringToObject(j, "version", app->version);
    cJSON_AddStringToObject(j, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(j, "board", ENTRANCE_BOARD);
    cJSON_AddStringToObject(j, "slot", p ? p->label : "unknown");
    cJSON_AddNumberToObject(j, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(j, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "minimum_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(j, "largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(j, "reset_reason", esp_reset_reason());
    cJSON_AddBoolToObject(j, "settings_healthy", settings_healthy);
    cJSON_AddBoolToObject(j, "rollback_available", esp_ota_check_rollback_is_possible());
    cJSON *slots = cJSON_AddArrayToObject(j, "slots");
    for (int i = 0; i < 2; i++) {
        const esp_partition_t *slot = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0 + i, NULL);
        if (!slot)
            continue;
        cJSON *item = cJSON_CreateObject();
        esp_app_desc_t description;
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        esp_ota_get_state_partition(slot, &state);
        const char *name = "undefined";
        switch (state) {
        case ESP_OTA_IMG_NEW:
            name = "new";
            break;
        case ESP_OTA_IMG_PENDING_VERIFY:
            name = "trial";
            break;
        case ESP_OTA_IMG_VALID:
            name = "valid";
            break;
        case ESP_OTA_IMG_INVALID:
            name = "invalid";
            break;
        case ESP_OTA_IMG_ABORTED:
            name = "aborted";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(item, "slot", slot->label);
        cJSON_AddStringToObject(item, "state", name);
        cJSON_AddBoolToObject(item, "running", slot == p);
        cJSON_AddNumberToObject(item, "address", slot->address);
        cJSON_AddNumberToObject(item, "capacity", slot->size);
        if (esp_ota_get_partition_description(slot, &description) == ESP_OK &&
            memchr(description.version, 0, sizeof(description.version)))
            cJSON_AddStringToObject(item, "version", description.version);
        else
            cJSON_AddStringToObject(item, "version", "empty");
        cJSON_AddItemToArray(slots, item);
    }
    cJSON_AddItemToObject(j, "network", net_status());
    cJSON_AddItemToObject(j, "ota", ota_status());
    return j;
}
