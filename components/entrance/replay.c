#include "replay.h"
#include <string.h>

static uint16_t u16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t u32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffff;
    while (n--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
static bool load_block(replay_t *r, unsigned block)
{
    if (block >= r->blocks)
        return false;
    const uint8_t *entry = r->data + r->index_offset + block * REPLAY_INDEX_BYTES;
    size_t offset = u32(entry + 4), compressed = u16(entry + 8), raw = u16(entry + 10);
    if (!r->inflate(r->data + offset, compressed, r->decoded, raw, r->inflate_context))
        return false;
    memcpy(r->scene, r->decoded, REPLAY_SCENE_BYTES);
    if (!replay_scene_valid(r->scene))
        return false;
    r->frame = u32(entry);
    r->block = block;
    r->block_frames = u16(entry + 12);
    r->local_frame = 0;
    r->cursor = REPLAY_SCENE_BYTES;
    r->block_size = raw;
    return r->block_frames != 1 || r->cursor == r->block_size;
}
static bool varint(const uint8_t *data, size_t end, size_t *at, unsigned *value)
{
    if (*at >= end)
        return false;
    unsigned a = data[(*at)++];
    *value = a & 127;
    if (a & 128) {
        if (*at >= end || data[*at] > 19)
            return false;
        *value |= (unsigned)data[(*at)++] << 7;
    }
    return *value <= REPLAY_SCENE_BYTES;
}
static bool step_in_block(replay_t *r)
{
    if (r->cursor + 2 > r->block_size || r->local_frame + 1 >= r->block_frames)
        return false;
    size_t end = r->cursor + 2 + u16(r->decoded + r->cursor);
    r->cursor += 2;
    if (end > r->block_size)
        return false;
    unsigned pos = 0;
    while (r->cursor < end) {
        unsigned skip, count;
        if (!varint(r->decoded, end, &r->cursor, &skip) ||
            !varint(r->decoded, end, &r->cursor, &count) || !count ||
            skip > REPLAY_SCENE_BYTES - pos || count > REPLAY_SCENE_BYTES - pos - skip ||
            count > end - r->cursor)
            return false;
        pos += skip;
        for (unsigned i = 0; i < count; i++)
            r->scene[pos++] += r->decoded[r->cursor++];
    }
    if (!replay_scene_valid(r->scene))
        return false;
    r->frame++;
    r->local_frame++;
    return r->local_frame + 1 < r->block_frames || r->cursor == r->block_size;
}
bool replay_open(replay_t *r, const uint8_t *data, size_t size, replay_inflate_t inflate,
                 void *context)
{
    memset(r, 0, sizeof(*r));
    if (!data || !inflate || size < REPLAY_HEADER_BYTES || size > REPLAY_MAX_BYTES ||
        memcmp(data, "RPL1", 4) || u32(data + 4) != size ||
        u32(data + 8) != crc32(data + 12, size - 12) || u16(data + 12) != 1 ||
        u16(data + 14) != REPLAY_SCENE_BYTES || u16(data + 24) != REPLAY_BLOCK_BYTES ||
        u16(data + 30))
        return false;
    r->frames = u32(data + 16);
    r->period_us = u32(data + 20);
    r->blocks = u16(data + 26);
    r->chapters = u16(data + 28);
    uint32_t tiles_offset = u32(data + 32), palette_offset = u32(data + 36);
    r->index_offset = u32(data + 40);
    r->chapters_offset = u32(data + 44);
    uint32_t payload = u32(data + 48);
    for (unsigned i = 52; i < 64; i++)
        if (data[i])
            return false;
    if (!r->frames || r->frames > 100000 || r->period_us < 50000 || r->period_us > 1000000 ||
        !r->blocks || r->blocks > r->frames || !r->chapters || r->chapters > 32 ||
        tiles_offset != 64 || palette_offset != tiles_offset + REPLAY_CHR_BYTES ||
        r->index_offset != palette_offset + 128 ||
        r->chapters_offset != r->index_offset + r->blocks * REPLAY_INDEX_BYTES ||
        payload != r->chapters_offset + r->chapters * 8 || payload > size)
        return false;
    uint32_t expected_frame = 0, expected_offset = payload;
    for (unsigned i = 0; i < r->blocks; i++) {
        const uint8_t *e = data + r->index_offset + i * REPLAY_INDEX_BYTES;
        unsigned compressed = u16(e + 8), raw = u16(e + 10), frames = u16(e + 12);
        if (u32(e) != expected_frame || u32(e + 4) != expected_offset || !compressed ||
            compressed > size - expected_offset || raw < REPLAY_SCENE_BYTES ||
            raw > REPLAY_BLOCK_BYTES || !frames || frames > 128 || u16(e + 14))
            return false;
        expected_frame += frames;
        expected_offset += compressed;
    }
    if (expected_frame != r->frames || expected_offset != size)
        return false;
    uint32_t previous = 0;
    for (unsigned i = 0; i < r->chapters; i++) {
        const uint8_t *ch = data + r->chapters_offset + i * 8;
        uint32_t frame = u32(ch);
        if (frame >= r->frames || (!i && frame) || (i && frame <= previous) || ch[4] > 7 ||
            ch[5] > 3 || ch[6] || ch[7])
            return false;
        previous = frame;
    }
    r->data = data;
    r->size = size;
    r->tiles = data + tiles_offset;
    r->palette = data + palette_offset;
    r->inflate = inflate;
    r->inflate_context = context;
    return load_block(r, 0);
}
bool replay_next(replay_t *r)
{
    if (!r->data)
        return false;
    if (r->frame + 1 == r->frames) {
        if (r->cursor != r->block_size || r->local_frame + 1 != r->block_frames)
            return false;
        r->loops++;
        return load_block(r, 0);
    }
    if (r->local_frame + 1 == r->block_frames)
        return r->cursor == r->block_size && load_block(r, r->block + 1);
    return step_in_block(r);
}
bool replay_seek(replay_t *r, uint32_t frame)
{
    if (!r->data || frame >= r->frames)
        return false;
    unsigned block = 0;
    for (unsigned i = 1; i < r->blocks; i++)
        if (u32(r->data + r->index_offset + i * REPLAY_INDEX_BYTES) <= frame)
            block = i;
    if (!load_block(r, block))
        return false;
    while (r->frame < frame)
        if (!step_in_block(r))
            return false;
    return true;
}
uint32_t replay_chapter_frame(const replay_t *r, unsigned chapter)
{
    return chapter < r->chapters ? u32(r->data + r->chapters_offset + chapter * 8) : UINT32_MAX;
}
uint16_t replay_color(const replay_t *r, unsigned index)
{
    return u16(r->palette + (index & 63) * 2);
}
