#include "wiz_protocol.h"
#include <string.h>

enum { UNKNOWN, RGB, TW, DW, SOCKET };
typedef struct { const char *module; uint8_t profile; } module_profile_t;

/* Exact upstream test-fixture module identifiers, not a substring heuristic.
 * Scenes are class profiles from pywizlight, not a claim that every proprietary
 * effect or configured custom/rhythm preset exists on a given bulb. */
static const module_profile_t profiles[] = {
    {"ESP01_SHRGB_03", RGB}, {"ESP01_SHRGB1C_31", RGB}, {"ESP24_SHRGB_01", RGB},
    {"ESP25_SHRGB_01", RGB}, {"ESP20_SHRGB_01BT", RGB}, {"ESP03_SHRGB3_01ABI", RGB},
    {"ESP20_SHRGB_01ABI", RGB}, {"ESP20_SHRGBC_01", RGB},
    {"ESP20_DHRGB_01", RGB}, {"ESP20_DHRGB_01B", RGB},
    {"ESP14_SHTW1C_01", TW}, {"ESP21_SHTW_01", TW}, {"ESP05_SHTW_21", TW},
    {"ESP05_SHDW_21", DW}, {"ESP06_SHDW9_01", DW},
    {"ESP25_SOCKET_01", SOCKET}, {"ESP10_SOCKET_06", SOCKET}
};

static void derive_profile(wiz_capabilities_t *caps)
{
    caps->profile = UNKNOWN;
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
        if (!strcmp(caps->module, profiles[i].module)) caps->profile = profiles[i].profile;
    caps->scene_count = 0;
    if (caps->profile == UNKNOWN) return;
    caps->evidence |= WIZ_EVIDENCE_CURATED_PROFILE;
    caps->features |= WIZ_POWER;
    if (caps->profile == SOCKET) { caps->features = WIZ_POWER; return; }
    caps->features |= WIZ_BRIGHTNESS | WIZ_SCENE | WIZ_SPEED;
    if (!caps->brightness_min) caps->brightness_min = 10;
    if (caps->profile == RGB) {
        caps->features |= WIZ_RGB;
        for (uint16_t n = 1; n <= 36; ++n) caps->scenes[caps->scene_count++] = n;
        caps->scenes[caps->scene_count++] = 40;
    } else {
        static const uint16_t tw[] = {6,9,10,11,12,13,14,15,16,18,29,30,31,32,33,35,40};
        static const uint16_t dw[] = {9,10,14,29,31,32,34,35};
        const uint16_t *values = caps->profile == TW ? tw : dw;
        size_t count = caps->profile == TW ? sizeof(tw) / sizeof(tw[0]) : sizeof(dw) / sizeof(dw[0]);
        for (size_t n = 0; n < count; ++n) caps->scenes[caps->scene_count++] = values[n];
    }
    if ((caps->profile == RGB || caps->profile == TW) &&
        caps->kelvin_min >= 1000 && caps->kelvin_max >= caps->kelvin_min &&
        caps->kelvin_max <= 10000) caps->features |= WIZ_TEMPERATURE;
}

void wiz_capabilities_merge(wiz_capabilities_t *caps, const wiz_reply_t *reply)
{
    if (!caps || !reply || reply->remote_error) return;
    const wiz_capabilities_t *delta = &reply->capability_delta;
    if (reply->method == WIZ_GET_SYSTEM) {
        /* A changed firmware/module must earn fresh range evidence. A new
         * reply missing its previous identity is also not a capability grant. */
        if ((caps->module[0] && strcmp(caps->module, delta->module)) ||
            (caps->firmware[0] && strcmp(caps->firmware, delta->firmware)))
            memset(caps, 0, sizeof(*caps));
        memcpy(caps->module, delta->module, sizeof(caps->module));
        memcpy(caps->firmware, delta->firmware, sizeof(caps->firmware));
    }
    caps->evidence |= delta->evidence;
    if (delta->kelvin_min) {
        caps->kelvin_min = delta->kelvin_min;
        caps->kelvin_max = delta->kelvin_max;
    }
    if (delta->brightness_min) caps->brightness_min = delta->brightness_min;
    if (reply->method == WIZ_GET_PILOT) {
        caps->features |= reply->state.fields & (WIZ_POWER | WIZ_BRIGHTNESS);
        if ((reply->state.fields & WIZ_BRIGHTNESS) && !caps->brightness_min) caps->brightness_min = 10;
    }
    derive_profile(caps);
}
