#pragma once
#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
typedef struct { size_t size; int index; } esp_partition_t;
typedef int esp_ota_handle_t;
typedef enum { ESP_OTA_IMG_UNDEFINED, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID } esp_ota_img_states_t;
#define ESP_PARTITION_TYPE_APP 0
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_ota_check_image_validity(int, const esp_image_header_t *, const esp_app_desc_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
bool esp_ota_check_rollback_is_possible(void);
