#include "media.h"
#include "core.h"
#include <string.h>

static uint16_t u16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t u32(const uint8_t *p)
{
    return u16(p) | (uint32_t)u16(p + 2) << 16;
}
uint8_t media_pixel(const uint8_t *packed, size_t pixel)
{
    return (packed[pixel / 2] >> (pixel & 1 ? 0 : 4)) & 15;
}
static void put(uint8_t *packed, size_t pixel, uint8_t color)
{
    uint8_t *p = packed + pixel / 2;
    *p = pixel & 1 ? (*p & 0xf0) | color : (*p & 0x0f) | (color << 4);
}
/* Validation and decoding deliberately share the exact token bounds. */
bool media_frame(const uint8_t *data, size_t size, size_t pixels, size_t *offset, uint8_t *packed)
{
    if (!data || !offset || !pixels || pixels > MEDIA_MAX_PIXELS || *offset > size ||
        size - *offset < 3)
        return false;
    size_t at = *offset, length = u16(data + at);
    at += 2;
    if (length < 1 || length > size - at)
        return false;
    size_t end = at + length;
    uint8_t mode = data[at++];
    if (mode == 0) {
        if (end - at != (pixels + 1) / 2 || (pixels & 1 && (data[end - 1] & 15)))
            return false;
        if (packed)
            memcpy(packed, data + at, end - at);
    } else if (mode == 1) {
        size_t pixel = 0;
        while (at < end) {
            uint8_t token = data[at++];
            size_t count = (token & 127) + 1;
            if (count > pixels - pixel)
                return false;
            if (token & 128) {
                if (at == end || data[at] > 15)
                    return false;
                uint8_t color = data[at++];
                if (packed)
                    for (size_t n = 0; n < count; n++)
                        put(packed, pixel + n, color);
            }
            pixel += count;
        }
        if (pixel != pixels)
            return false;
    } else {
        return false;
    }
    *offset = end;
    return true;
}
bool media_validate(const uint8_t *data, size_t size, media_info_t *info)
{
    if (!data || size < MEDIA_HEADER_BYTES + 3 || size > MEDIA_MAX_BYTES ||
        memcmp(data, "ECL1", 4) || u32(data + 4) != size || data[15] || data[18] || data[19])
        return false;
    unsigned w = data[12], h = data[13], fps = data[14], frames = u16(data + 16);
    if (!w || !h || w > 160 || h > 160 || w * h > MEDIA_MAX_PIXELS || !fps || fps > 12 || !frames ||
        frames > MEDIA_MAX_FRAMES || frames > fps * 60 || data[47] || data[82] != 0 ||
        record_crc(data + 12, size - 12) != u32(data + 8))
        return false;
    /* Printable ASCII labels only; UI content itself is rasterized, including Unicode. */
    bool ended = false;
    for (unsigned i = 20; i < 48; i++) {
        if (!data[i])
            ended = true;
        else if (ended || data[i] < 32 || data[i] > 126)
            return false;
    }
    size_t at = MEDIA_HEADER_BYTES;
    for (unsigned i = 0; i < frames; i++)
        if (!media_frame(data, size, w * h, &at, NULL))
            return false;
    if (at != size)
        return false;
    if (info) {
        *info = (media_info_t){
            .width = w, .height = h, .fps = fps, .frames = frames, .crc = u32(data + 8)};
        memcpy(info->title, data + 20, sizeof(info->title));
    }
    return true;
}
