#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DISPLAY_CONFIG_MAGIC 0x32444345u
#define ANIMATION_SLOTS 2u
typedef struct {
    uint32_t magic;
    uint8_t mode, rotation, profile, inverted;
    uint8_t animation_slot, reserved[3];
    uint32_t crc;
} display_config_t;
bool display_config_valid(const display_config_t *value);
bool display_config_legacy(const void *data, size_t size, display_config_t *out);
