#include "replay.h"
#include <string.h>
#include <zlib.h>

static replay_t player;
static bool inflate_block(const uint8_t *source, size_t size, uint8_t *out, size_t expected,
                          void *context)
{
    (void)context;
    uLongf output = expected;
    uLong input = size;
    return uncompress2(out, &output, source, &input) == Z_OK && output == expected && input == size;
}
int open_replay(const uint8_t *source, size_t size)
{
    return replay_open(&player, source, size, inflate_block, NULL);
}
int advance(void)
{
    return replay_next(&player);
}
int seek_frame(unsigned frame)
{
    return replay_seek(&player, frame);
}
unsigned current_frame(void)
{
    return player.frame;
}
unsigned loop_count(void)
{
    return player.loops;
}
void get_scene(uint8_t *out)
{
    memcpy(out, player.scene, REPLAY_SCENE_BYTES);
}
void get_frame(uint8_t *out)
{
    replay_scene_frame(player.scene, player.tiles, out);
}
