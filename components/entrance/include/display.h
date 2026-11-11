#pragma once
#include "entrance.h"
#include "media.h"

esp_err_t display_start(void);
bool display_healthy(void);
cJSON *display_status(void);
esp_err_t display_install(const uint8_t *data, size_t size);
esp_err_t display_remove(void);
esp_err_t display_configure(const cJSON *request);
/* Copy while locked; caller owns and frees the returned bytes. */
uint8_t *display_copy(size_t *size);
