/* Compile/link-only target. Never flashed by this test workflow. */
#include "wiz_esp_idf.h"
#include <assert.h>

void app_main(void)
{
    static wiz_driver_t driver;
    static wiz_esp_idf_t adapter;
    wiz_io_t io = {0};
    /* References prove adapter and crypto compile against real ESP-IDF APIs;
     * deliberately invalid inputs guarantee no socket or network mutation. */
    assert(wiz_esp_idf_init(&adapter, NULL, NULL, NULL, &io) == WIZ_ARGUMENT);
    assert(wiz_driver_init(&driver, &io) == WIZ_ARGUMENT);
    wiz_key_t key = {0};
    char hmac[45];
    assert(wiz_hmac_base64(&key, NULL, 0, hmac) == WIZ_KEY_REQUIRED);
}
