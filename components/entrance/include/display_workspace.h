#pragma once
#include "media.h"
#include <string.h>

/* Restore a validated delta clip after games borrowed its pixel workspace.
 * Called only while the display mutex is held. No allocation occurs here. */
static inline bool display_workspace_seek_clip(const uint8_t *data, size_t size,
                                               const media_info_t *info, uint16_t frame,
                                               uint8_t pixels[MEDIA_MAX_PIXELS / 2],
                                               size_t *offset)
{
    if (!data || !info || !pixels || !offset || !info->width || !info->height ||
        (size_t)info->width * info->height > MEDIA_MAX_PIXELS || frame >= info->frames)
        return false;
    memset(pixels, 0, MEDIA_MAX_PIXELS / 2);
    size_t at = MEDIA_HEADER_BYTES;
    for (uint32_t i = 0; i <= frame; ++i)
        if (!media_frame(data, size, (size_t)info->width * info->height, &at, pixels))
            return false;
    *offset = at;
    return true;
}
