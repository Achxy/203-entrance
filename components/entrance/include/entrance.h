#pragma once
#include "core.h"
#include "esp_err.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <stdatomic.h>

typedef struct {
    char ssid[33], ap_password[65], device_id[24];
    char *certificate, *private_key, *signing_public;
    uint8_t admin_salt[16], admin_hash[32];
} identity_t;
typedef struct {
    uint32_t magic, image_size;
    uint8_t image_hash[32], ticket_hash[32];
    char version[32], result[24], detail[96];
    uint32_t crc;
} update_record_t;

extern identity_t identity;
extern settings_t settings;
extern _Atomic bool settings_healthy;
extern SemaphoreHandle_t operation_mutex;
esp_err_t store_init(void);
esp_err_t store_settings(const char *ssid, const char *password, bool enabled);
settings_t settings_snapshot(void);
esp_err_t store_update(update_record_t *record);
esp_err_t load_update(update_record_t *record);
bool check_password(const char *password);
void sha256_bytes(const void *data, size_t len, uint8_t out[32]);
void random_hex(char *out, size_t bytes);

void diag_init(void);
cJSON *diag_status(void);
cJSON *diag_logs(uint32_t after);
void diag_event(const char *message);

esp_err_t net_init(void);
esp_err_t net_apply(void);
bool net_online(void);
bool net_ready(void);
void net_maintenance(void);
cJSON *net_status(void);

void ota_init(void);
bool ota_busy(void);
bool ota_trial(void);
esp_err_t ota_begin(size_t size, const uint8_t hash[32], const char *version, char ticket[65]);
esp_err_t ota_write(size_t offset, const void *data, size_t len);
esp_err_t ota_finish(void);
void ota_abort(const char *detail);
esp_err_t ota_confirm(const char *ticket);
bool ota_ticket_valid(const char *ticket);
cJSON *ota_status(void);
void ota_tick(bool healthy);
esp_err_t ota_rollback(void);
void ota_schedule_reboot(void);

esp_err_t api_start(void);
bool api_ready(void);
void health_start(void);
void diag_health_sample(bool healthy);
cJSON *diag_soak(void);
void health_set_services(bool ready);

void release_start(void);
bool release_checking(void);
esp_err_t release_check(const char *version);
esp_err_t release_install(char ticket[65]);
cJSON *release_status(void);
