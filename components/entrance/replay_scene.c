#include "replay_scene.h"
#include <string.h>

static unsigned u16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}
static unsigned tile_pixel(const uint8_t *tiles, unsigned tile, unsigned x, unsigned y)
{
    const uint8_t *p = tiles + tile * 16 + y;
    return ((p[0] >> (7 - x)) & 1) | (((p[8] >> (7 - x)) & 1) << 1);
}
bool replay_scene_valid(const uint8_t *s)
{
    unsigned count = s[2404];
    if (!count || count > REPLAY_MAX_SPANS || s[2340] || s[2405] > 7 || s[2406] > 3)
        return false;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *p = s + 2340 + i * 8;
        if (p[0] >= 240 || (i && p[0] <= p[-8]) || u16(p + 2) >= 512 || u16(p + 4) >= 240 ||
            (p[7] & 0xe0))
            return false;
    }
    for (unsigned i = 2304; i < 2336; i++)
        if (s[i] > 63)
            return false;
    return true;
}
void replay_scene_row(const uint8_t *s, const uint8_t *tiles, unsigned y, uint8_t out[256])
{
    const uint8_t *span = s + 2340, *palette = s + 2304;
    for (unsigned i = 1; i < s[2404]; i++)
        if (s[2340 + i * 8] <= y)
            span = s + 2340 + i * 8;
    unsigned scroll = u16(span + 2), sy = (u16(span + 4) + y - span[0]) % 240;
    unsigned ctrl = span[6], mask = span[7];
    uint8_t opaque[256];
    memset(opaque, 0, sizeof(opaque));
    for (unsigned x = 0; x < 256; x++) {
        unsigned color = 0;
        if ((mask & 8) && (x >= 8 || (mask & 2))) {
            unsigned sx = (x + scroll) & 511, nt = (sx >> 8) * 1024;
            unsigned tx = (sx & 255) >> 3, ty = sy >> 3;
            unsigned tile = s[nt + ty * 32 + tx] + ((ctrl & 16) ? 256 : 0);
            unsigned pixel = tile_pixel(tiles, tile, sx & 7, sy & 7);
            if (pixel) {
                unsigned attr = s[nt + 960 + (ty >> 2) * 8 + (tx >> 2)];
                unsigned bank = (attr >> ((ty & 2) * 2 + (tx & 2))) & 3;
                color = bank * 4 + pixel;
                opaque[x] = 1;
            }
        }
        out[x] = palette[color];
    }
    if (mask & 16) {
        uint8_t occupied[256] = {0};
        unsigned count = 0, height = (ctrl & 32) ? 16 : 8;
        for (unsigned i = 0; i < 64 && count < 8; i++) {
            const uint8_t *sprite = s + 2048 + i * 4;
            int dy = (int)y - sprite[0] - 1;
            if (dy < 0 || (unsigned)dy >= height)
                continue;
            count++;
            unsigned attr = sprite[2], row = (attr & 128) ? height - 1 - dy : (unsigned)dy;
            unsigned tile = sprite[1];
            if (height == 16)
                tile = (tile & 254) + ((tile & 1) ? 256 : 0) + (row >> 3);
            else
                tile += (ctrl & 8) ? 256 : 0;
            for (unsigned dx = 0; dx < 8 && sprite[3] + dx < 256; dx++) {
                unsigned x = sprite[3] + dx;
                if (occupied[x] || (x < 8 && !(mask & 4)))
                    continue;
                unsigned pixel = tile_pixel(tiles, tile, (attr & 64) ? 7 - dx : dx, row & 7);
                if (!pixel)
                    continue;
                occupied[x] = 1;
                if (!(attr & 32) || !opaque[x])
                    out[x] = palette[16 + (attr & 3) * 4 + pixel];
            }
        }
    }
    if (mask & 1)
        for (unsigned x = 0; x < 256; x++)
            out[x] &= 0x30;
}
void replay_scene_frame(const uint8_t *s, const uint8_t *tiles, uint8_t out[256 * 240])
{
    for (unsigned y = 0; y < 240; y++)
        replay_scene_row(s, tiles, y, out + y * 256);
}
