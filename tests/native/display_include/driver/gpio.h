#pragma once
#include "esp_err.h"
#define GPIO_MODE_OUTPUT 1
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
static inline int gpio_set_level(int pin, int value) { (void)pin; (void)value; return 0; }
static inline esp_err_t gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }
