#pragma once
#include "media.h"

/* The signed built-in animation uses the same bounded codec as custom clips. */
typedef struct {
    const uint8_t *data;
    size_t size, offset;
    media_info_t info;
    uint32_t frame, loops, period_us;
    uint8_t pixels[MEDIA_MAX_PIXELS / 2];
} builtin_loop_t;

bool builtin_loop_open(builtin_loop_t *loop, const uint8_t *data, size_t size);
bool builtin_loop_next(builtin_loop_t *loop);
bool builtin_loop_seek(builtin_loop_t *loop, uint32_t frame);
uint16_t builtin_loop_color(const builtin_loop_t *loop, size_t pixel);
bool builtin_loop_row(const builtin_loop_t *loop, unsigned width, unsigned height,
                      unsigned y, unsigned caption, uint16_t *out);
