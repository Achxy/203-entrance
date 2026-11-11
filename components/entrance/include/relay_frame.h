#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define RELAY_INPUT_MAX 2048
/* Handles transport buffer chunks and RFC6455 continuation frames separately. */
typedef struct {
    char bytes[RELAY_INPUT_MAX + 1];
    size_t used, frame_start, offset, frame_length;
    bool active;
} relay_frame_t;
/* -1 invalid, 0 incomplete/control frame, 1 complete UTF-8 JSON bytes. */
static inline int relay_frame_feed(relay_frame_t *f, unsigned opcode, bool fin, int total,
                                   int offset, const char *data, int length)
{
    if (opcode >= 8)
        return 0;
    if (total < 0 || offset < 0 || length < 0 || offset > total || length > total - offset ||
        total > RELAY_INPUT_MAX || (length && !data))
        return -1;
    if (!offset) {
        if (opcode == 1) {
            if (f->active)
                return -1;
            f->used = 0;
            f->active = true;
        } else if (opcode != 0 || !f->active)
            return -1;
        f->frame_start = f->used;
        f->offset = 0;
        f->frame_length = (size_t)total;
    }
    if (!f->active || (opcode != 0 && opcode != 1) || (size_t)offset != f->offset ||
        (size_t)total != f->frame_length || f->frame_start + (size_t)total > RELAY_INPUT_MAX)
        return -1;
    if (length)
        memcpy(f->bytes + f->used, data, (size_t)length);
    f->used += (size_t)length;
    f->offset += (size_t)length;
    if (offset + length == total && fin) {
        f->bytes[f->used] = 0;
        f->active = false;
        return 1;
    }
    return 0;
}
