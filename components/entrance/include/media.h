#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fits two NVS blob generations plus the OTA journal and GC reserve in state. */
#define MEDIA_MAX_BYTES 12288u
#define MEDIA_HEADER_BYTES 80u
#define MEDIA_MAX_PIXELS 20480u
#define MEDIA_MAX_FRAMES 120u
typedef struct {
    uint8_t width, height, fps;
    uint16_t frames;
    uint32_t crc;
    char title[28];
} media_info_t;

bool media_validate(const uint8_t *data, size_t size, media_info_t *info);
/* Caller supplies MEDIA_MAX_PIXELS/2 bytes. Start offset at MEDIA_HEADER_BYTES. */
bool media_frame(const uint8_t *data, size_t size, size_t pixels, size_t *offset, uint8_t *packed);
uint8_t media_pixel(const uint8_t *packed, size_t pixel);
