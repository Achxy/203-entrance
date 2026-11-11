#pragma once
#include "replay_scene.h"

#define REPLAY_BLOCK_BYTES 16384
#define REPLAY_HEADER_BYTES 64
#define REPLAY_INDEX_BYTES 16
#define REPLAY_MAX_BYTES 737280

typedef bool (*replay_inflate_t)(const uint8_t *, size_t, uint8_t *, size_t, void *);
typedef struct {
    const uint8_t *data, *tiles, *palette;
    size_t size, cursor, block_size;
    uint32_t frames, period_us, frame, index_offset, chapters_offset, loops;
    uint16_t blocks, chapters, block, block_frames, local_frame;
    replay_inflate_t inflate;
    void *inflate_context;
    uint8_t scene[REPLAY_SCENE_BYTES], decoded[REPLAY_BLOCK_BYTES];
} replay_t;

bool replay_open(replay_t *r, const uint8_t *data, size_t size, replay_inflate_t inflate,
                 void *context);
bool replay_next(replay_t *r);
bool replay_seek(replay_t *r, uint32_t frame);
uint32_t replay_chapter_frame(const replay_t *r, unsigned chapter);
uint16_t replay_color(const replay_t *r, unsigned index);
