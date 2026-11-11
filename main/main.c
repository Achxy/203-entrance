#include "entrance.h"
#include "display.h"
#include "esp_log.h"

void app_main(void)
{
    diag_init();
    esp_err_t err = store_init();
    if (err != ESP_OK) {
        ESP_LOGE("boot",
                 "Identity unavailable (%s). Restore identity with entrance provision over USB.",
                 esp_err_to_name(err));
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_err_t rollback = esp_ota_mark_app_invalid_rollback_and_reboot();
            ESP_LOGE("boot", "Trial recovery could not reboot: %s", esp_err_to_name(rollback));
        }
        return;
    }
    ota_init();
    health_start(); /* Trial deadline is armed before networking starts. */
    err = net_init();
    if (err == ESP_OK)
        err = api_start();
    if (err == ESP_OK)
        err = display_start();
    health_set_services(err == ESP_OK);
    if (err != ESP_OK) {
        ESP_LOGE("boot", "Management startup failed: %s", esp_err_to_name(err));
        return;
    }
    release_start();
    ESP_LOGI("boot", "Management ready. Open https://192.168.4.1 on the device hotspot.");
}
