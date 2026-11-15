#ifndef WIZ_PROTOCOL_H
#define WIZ_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIZ_DATAGRAM_MAX 2048u
#define WIZ_MAX_BULBS 8u
#define WIZ_PORT 38899u
#define WIZ_SCENE_COUNT 40u

typedef enum {
    WIZ_OK = 0, WIZ_ARGUMENT, WIZ_TOO_LARGE, WIZ_MALFORMED, WIZ_UNSUPPORTED,
    WIZ_WRONG_METHOD, WIZ_WRONG_IDENTITY, WIZ_KEY_REQUIRED, WIZ_CLOCK_REQUIRED,
    WIZ_CRYPTO_FAILED, WIZ_REJECTED, WIZ_BUSY, WIZ_NOT_ENROLLED, WIZ_NO_PEER,
    WIZ_PEER_CHANGED, WIZ_IO, WIZ_TIMEOUT, WIZ_CANCELLED, WIZ_UNCONFIRMED,
    WIZ_CLOCK_ROLLBACK, WIZ_IDLE
} wiz_error_t;

typedef enum {
    WIZ_GET_PILOT = 0, WIZ_GET_SYSTEM, WIZ_GET_MODEL, WIZ_GET_USER,
    WIZ_GET_POWER, WIZ_SET_PILOT
} wiz_method_t;

enum {
    WIZ_POWER = 1u, WIZ_BRIGHTNESS = 2u, WIZ_RGB = 4u,
    WIZ_TEMPERATURE = 8u, WIZ_SCENE = 16u, WIZ_SPEED = 32u
};

/* An absolute, canonical patch. Unselected fields must be zero. Brightness is
 * wire percent, not pywizlight's convenience 0..255 UI representation. */
typedef struct {
    uint16_t fields, kelvin, scene;
    uint8_t power, brightness, red, green, blue, speed;
} wiz_patch_t;

enum {
    WIZ_EVIDENCE_PILOT = 1u, WIZ_EVIDENCE_SYSTEM = 2u,
    WIZ_EVIDENCE_MODEL = 4u, WIZ_EVIDENCE_USER = 8u,
    WIZ_EVIDENCE_CURATED_PROFILE = 16u
};

/* Features are the conservative intersection of actual replies and a small
 * exact module-name profile set. Unknown models do not inherit RGB/CCT/scenes. */
typedef struct {
    uint16_t features, evidence, kelvin_min, kelvin_max;
    uint8_t brightness_min;
    uint8_t profile; /* Internal: unknown, RGB, tunable white, dimmable, socket. */
    uint8_t scene_count;
    uint16_t scenes[WIZ_SCENE_COUNT];
    char module[48], firmware[24];
} wiz_capabilities_t;

typedef struct {
    uint8_t bytes[16];
    bool present;
} wiz_key_t;

typedef struct {
    bool signing_required;
    wiz_key_t key;
} wiz_security_t;

/* The integrator supplies a time sample only after a trusted clock has been
 * synchronized. Monotonic request deadlines are separate from signing time. */
typedef struct { uint64_t unix_seconds; bool trusted; } wiz_wall_clock_t;

typedef struct {
    wiz_method_t method;
    bool accepted;
    bool identity_present;
    bool signature_present; /* Informational only: NOT signature verification. */
    bool remote_error;
    int32_t remote_error_code;
    uint8_t mac[6];
    wiz_patch_t state;
    wiz_capabilities_t capability_delta;
    bool has_power_mw;
    uint32_t power_mw;
} wiz_reply_t;

/* Accept only raw 12 hex or six colon-separated octets. Reject multicast,
 * broadcast, all-zero MACs. Parse is transactional on failure. */
wiz_error_t wiz_parse_mac(const char *text, uint8_t out[6]);
wiz_error_t wiz_parse_key(const char *hex, wiz_key_t *out);
void wiz_key_clear(wiz_key_t *key);
const char *wiz_method_name(wiz_method_t method);
const char *wiz_error_name(wiz_error_t error);
bool wiz_scene_supported(const wiz_capabilities_t *caps, uint16_t scene);
wiz_error_t wiz_validate_patch(const wiz_patch_t *patch,
                               const wiz_capabilities_t *caps);
bool wiz_state_matches(const wiz_patch_t *desired, const wiz_patch_t *observed);
void wiz_capabilities_merge(wiz_capabilities_t *caps, const wiz_reply_t *reply);

/* Compact params are emitted once and those exact bytes are HMAC'd. Required
 * signing with a missing key/clock fails closed; no retry can become unsigned. */
wiz_error_t wiz_encode_request(wiz_method_t method, const wiz_patch_t *patch,
                               const wiz_capabilities_t *caps,
                               const wiz_security_t *security,
                               wiz_wall_clock_t clock,
                               char *out, size_t capacity, size_t *written);
wiz_error_t wiz_hmac_base64(const wiz_key_t *key, const uint8_t *data,
                           size_t size, char out[45]);

/* Expected method and enrolled MAC are supplied by the request tracker, never
 * by a remote client. A config/ack may omit MAC; the driver still checks source
 * peer plus AP association and DHCP mapping. Replies are unverified LAN data. */
wiz_error_t wiz_parse_reply(const uint8_t *data, size_t size,
                            wiz_method_t expected_method,
                            const uint8_t expected_mac[6], wiz_reply_t *out);

#endif
