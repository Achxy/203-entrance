#include "display_config.h"
#include "core.h"
#include <string.h>
static bool panel_valid(uint8_t mode, uint8_t rotation, uint8_t profile, uint8_t inverted)
{
    return mode < 6 && rotation < 4 && profile < 3 && inverted < 2;
}
bool display_config_valid(const display_config_t *v)
{
    return v && v->magic == DISPLAY_CONFIG_MAGIC &&
           panel_valid(v->mode, v->rotation, v->profile, v->inverted) &&
           v->animation_slot >= 1 && v->animation_slot <= ANIMATION_SLOTS &&
           !v->reserved[0] && !v->reserved[1] && !v->reserved[2] &&
           v->crc == record_crc(v, offsetof(display_config_t, crc));
}
bool display_config_legacy(const void *data, size_t size, display_config_t *out)
{
    struct { uint32_t magic; uint8_t mode, rotation, profile, inverted; uint32_t crc; } old;
    if (!data || !out || size != sizeof(old)) return false;
    memcpy(&old, data, sizeof(old));
    if (old.magic != 0x31444345 || !panel_valid(old.mode, old.rotation, old.profile, old.inverted) ||
        old.crc != record_crc(&old, offsetof(__typeof__(old), crc))) return false;
    *out = (display_config_t){.magic = DISPLAY_CONFIG_MAGIC, .mode = old.mode,
        .rotation = old.rotation, .profile = old.profile, .inverted = old.inverted, .animation_slot = 1};
    out->crc = record_crc(out, offsetof(display_config_t, crc));
    return true;
}
