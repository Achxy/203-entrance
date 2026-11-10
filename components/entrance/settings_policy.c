#include "core.h"
#include <string.h>

bool settings_valid(const settings_t *s)
{
    return s && s->schema == ENTRANCE_SCHEMA && s->enabled <= 1 &&
           memchr(s->ssid, 0, sizeof(s->ssid)) && memchr(s->password, 0, sizeof(s->password)) &&
           (!s->enabled || (*s->ssid && (!*s->password || strlen(s->password) >= 8))) &&
           s->crc == record_crc(s, offsetof(settings_t, crc));
}

bool settings_select(const settings_t *a, bool read_a, const settings_t *b, bool read_b,
                     settings_t *out)
{
    bool av = read_a && settings_valid(a), bv = read_b && settings_valid(b);
    if (!av && !bv)
        return false;
    *out = av && (!bv || a->generation >= b->generation) ? *a : *b;
    return true;
}
