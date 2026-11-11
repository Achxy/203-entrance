#include "builtin_loop.h"
#include "captions.h"
#include <string.h>

bool builtin_loop_seek(builtin_loop_t *loop, uint32_t frame)
{
    if (!loop || !loop->data || frame >= loop->info.frames)
        return false;
    loop->offset = MEDIA_HEADER_BYTES;
    memset(loop->pixels, 0, sizeof(loop->pixels));
    for (uint32_t i = 0; i <= frame; i++)
        if (!media_frame(loop->data, loop->size, loop->info.width * loop->info.height,
                         &loop->offset, loop->pixels))
            return false;
    loop->frame = frame;
    return true;
}
bool builtin_loop_open(builtin_loop_t *loop, const uint8_t *data, size_t size)
{
    if (!loop)
        return false;
    memset(loop, 0, sizeof(*loop));
    if (!media_validate(data, size, &loop->info))
        return false;
    loop->data = data;
    loop->size = size;
    loop->period_us = 1000000 / loop->info.fps;
    return builtin_loop_seek(loop, 0);
}
bool builtin_loop_next(builtin_loop_t *loop)
{
    if (!loop || !loop->data)
        return false;
    if (loop->frame + 1 == loop->info.frames) {
        if (loop->offset != loop->size || !builtin_loop_seek(loop, 0))
            return false;
        loop->loops++;
        return true;
    }
    if (!media_frame(loop->data, loop->size, loop->info.width * loop->info.height,
                     &loop->offset, loop->pixels))
        return false;
    loop->frame++;
    return true;
}
uint16_t builtin_loop_color(const builtin_loop_t *loop, size_t pixel)
{
    if (!loop || !loop->data || pixel >= (size_t)loop->info.width * loop->info.height)
        return 0;
    unsigned at = 48 + media_pixel(loop->pixels, pixel) * 2;
    return loop->data[at] | (uint16_t)loop->data[at + 1] << 8;
}
bool builtin_loop_row(const builtin_loop_t *loop, unsigned width, unsigned height,
                      unsigned y, unsigned caption, uint16_t *out)
{
    if (!loop || !loop->data || !out || y >= height ||
        !((width == 160 && height == 128) || (width == 128 && height == 160)))
        return false;
    unsigned header = height == 128 ? CAPTION_HEADER_LANDSCAPE : CAPTION_HEADER_PORTRAIT;
    if (y < header) {
        caption_row(caption, width, header, y, out);
        return true;
    }
    memset(out, 0, width * sizeof(*out));
    if (y == header) {
        for (unsigned x = 0; x < width; x++)
            out[x] = 0xffe0;
        return true;
    }
    unsigned dw = width, dh = height - header - 1;
    if (width * loop->info.height > dh * loop->info.width)
        dw = dh * loop->info.width / loop->info.height;
    else
        dh = width * loop->info.height / loop->info.width;
    if (!dw) dw = 1;
    if (!dh) dh = 1;
    unsigned left = (width - dw) / 2, top = header + 1 + (height - header - 1 - dh) / 2;
    if (y >= top && y < top + dh)
        for (unsigned x = 0; x < dw; x++) {
            unsigned sx = x * loop->info.width / dw;
            unsigned sy = (y - top) * loop->info.height / dh;
            out[left + x] = builtin_loop_color(loop, sy * loop->info.width + sx);
        }
    return true;
}
