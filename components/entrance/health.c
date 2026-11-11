#include "entrance.h"
#include "display.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <stdlib.h>

static _Atomic bool services;
void health_set_services(bool ready)
{
    services = ready;
}
static void health_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << 9, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&button);
    int64_t pressed_at = 0;
    int64_t telemetry_at = 0;
    bool handled = false;
    bool storage_maintenance = false;
    for (;;) {
        if (!settings_healthy && net_ready() && !storage_maintenance) {
            ota_abort("Storage error: update cancelled; maintenance required");
            net_maintenance();
            storage_maintenance = true;
        }
        bool healthy = services && net_ready() && api_ready() && display_healthy() &&
                       settings_healthy && esp_get_free_heap_size() > 24000;
#if ENTRANCE_TEST_FAULT == 1
        if (ota_trial())
            healthy = false;
#endif
        ota_tick(healthy);
        int64_t now = esp_timer_get_time();
        if (now >= telemetry_at) {
            telemetry_at = now + 15000000;
            ESP_LOGI("health",
                     "HEALTH "
                     "{\"version\":\"%s\",\"uptime_ms\":%lld,\"free_heap\":%lu,\"minimum_heap\":%"
                     "lu,\"largest_block\":%u,\"reset_reason\":%d,\"services\":%s,\"trial\":%s}",
                     esp_app_get_description()->version, (long long)(now / 1000),
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     esp_reset_reason(), healthy ? "true" : "false",
                     ota_trial() ? "true" : "false");
        }
        if (!gpio_get_level(9)) {
            if (!pressed_at)
                pressed_at = now;
            if (!handled && now - pressed_at >= 5000000) {
                ota_abort("Transfer cancelled by physical maintenance request");
                net_maintenance();
                handled = true;
            }
        } else {
            pressed_at = 0;
            handled = false;
        }
#if ENTRANCE_TEST_FAULT == 2
        if (!ota_trial() || now < 10000000)
            esp_task_wdt_reset();
#else
        esp_task_wdt_reset();
#endif
#if ENTRANCE_TEST_FAULT == 3
        if (ota_trial() && now >= 10000000)
            esp_restart();
#endif
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
void health_start(void)
{
    if (xTaskCreate(health_task, "health", 4096, NULL, 4, NULL) != pdPASS)
        abort();
}
