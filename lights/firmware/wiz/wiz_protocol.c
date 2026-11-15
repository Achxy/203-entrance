#include "wiz_protocol.h"
#include <cJSON.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static size_t bounded_length(const char *s, size_t max)
{
    size_t n = 0;
    if (!s) return max;
    while (n < max && s[n]) ++n;
    return n;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

wiz_error_t wiz_parse_mac(const char *s, uint8_t out[6])
{
    if (!s || !out) return WIZ_ARGUMENT;
    size_t length = bounded_length(s, 18);
    if (length != 12 && length != 17) return WIZ_ARGUMENT;
    uint8_t parsed[6];
    unsigned any = 0;
    for (unsigned i = 0; i < 6; ++i) {
        size_t p = length == 12 ? i * 2 : i * 3;
        int a = hex_digit(s[p]), b = hex_digit(s[p + 1]);
        if (a < 0 || b < 0 || (length == 17 && i < 5 && s[p + 2] != ':'))
            return WIZ_ARGUMENT;
        parsed[i] = (uint8_t)((a << 4) | b);
        any |= parsed[i];
    }
    if (!any || (parsed[0] & 1u)) return WIZ_ARGUMENT;
    memcpy(out, parsed, sizeof(parsed));
    return WIZ_OK;
}

wiz_error_t wiz_parse_key(const char *s, wiz_key_t *out)
{
    if (!out) return WIZ_ARGUMENT;
    wiz_key_clear(out);
    if (!s || bounded_length(s, 33) != 32) return WIZ_ARGUMENT;
    wiz_key_t candidate = {0};
    for (unsigned i = 0; i < 16; ++i) {
        int a = hex_digit(s[2 * i]), b = hex_digit(s[2 * i + 1]);
        if (a < 0 || b < 0) {
            wiz_key_clear(&candidate);
            return WIZ_ARGUMENT;
        }
        candidate.bytes[i] = (uint8_t)((a << 4) | b);
    }
    candidate.present = true;
    *out = candidate;
    wiz_key_clear(&candidate);
    return WIZ_OK;
}

const char *wiz_method_name(wiz_method_t method)
{
    static const char *const names[] = {"getPilot", "getSystemConfig",
        "getModelConfig", "getUserConfig", "getPower", "setPilot"};
    return (unsigned)method < sizeof(names) / sizeof(names[0]) ? names[method] : NULL;
}

const char *wiz_error_name(wiz_error_t error)
{
    static const char *const names[] = {"ok", "argument", "too_large", "malformed",
        "unsupported", "wrong_method", "wrong_identity", "key_required",
        "clock_required", "crypto_failed", "rejected", "busy", "not_enrolled",
        "no_peer", "peer_changed", "io", "timeout", "cancelled", "unconfirmed",
        "clock_rollback", "idle"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

bool wiz_scene_supported(const wiz_capabilities_t *caps, uint16_t scene)
{
    if (!caps || !(caps->features & WIZ_SCENE) ||
        caps->scene_count > WIZ_SCENE_COUNT || scene == 0) return false;
    for (unsigned i = 0; i < caps->scene_count; ++i)
        if (caps->scenes[i] == scene) return true;
    return false;
}

wiz_error_t wiz_validate_patch(const wiz_patch_t *p, const wiz_capabilities_t *c)
{
    const unsigned all = WIZ_POWER | WIZ_BRIGHTNESS | WIZ_RGB |
                         WIZ_TEMPERATURE | WIZ_SCENE | WIZ_SPEED;
    if (!p || !c || !p->fields || (p->fields & ~all)) return WIZ_ARGUMENT;
    if (p->fields & ~c->features) return WIZ_UNSUPPORTED;
    unsigned modes = p->fields & (WIZ_RGB | WIZ_TEMPERATURE | WIZ_SCENE);
    if (modes && (modes & (modes - 1))) return WIZ_ARGUMENT;
    if ((p->fields & WIZ_POWER) && p->power > 1) return WIZ_ARGUMENT;
    if ((p->fields & WIZ_POWER) && !p->power && p->fields != WIZ_POWER)
        return WIZ_ARGUMENT;
    if (!(p->fields & WIZ_POWER) && p->power) return WIZ_ARGUMENT;
    if (!(p->fields & WIZ_BRIGHTNESS) && p->brightness) return WIZ_ARGUMENT;
    if (!(p->fields & WIZ_RGB) && (p->red || p->green || p->blue)) return WIZ_ARGUMENT;
    if (!(p->fields & WIZ_TEMPERATURE) && p->kelvin) return WIZ_ARGUMENT;
    if (!(p->fields & WIZ_SCENE) && p->scene) return WIZ_ARGUMENT;
    if (!(p->fields & WIZ_SPEED) && p->speed) return WIZ_ARGUMENT;
    if ((p->fields & WIZ_BRIGHTNESS) &&
        (c->brightness_min < 1 || c->brightness_min > 100 ||
         p->brightness < c->brightness_min || p->brightness > 100)) return WIZ_ARGUMENT;
    if ((p->fields & WIZ_TEMPERATURE) &&
        (c->kelvin_min < 1000 || c->kelvin_max > 10000 ||
         c->kelvin_min > c->kelvin_max || p->kelvin < c->kelvin_min ||
         p->kelvin > c->kelvin_max)) return WIZ_ARGUMENT;
    if ((p->fields & WIZ_SCENE) && !wiz_scene_supported(c, p->scene)) return WIZ_UNSUPPORTED;
    /* Speed without an explicit, supported scene is ambiguous and stale-prone. */
    if ((p->fields & WIZ_SPEED) &&
        (!(p->fields & WIZ_SCENE) || p->speed < 10 || p->speed > 200)) return WIZ_ARGUMENT;
    return WIZ_OK;
}

bool wiz_state_matches(const wiz_patch_t *d, const wiz_patch_t *o)
{
    if (!d || !o || !d->fields || (d->fields & o->fields) != d->fields) return false;
    if ((d->fields & WIZ_POWER) && d->power != o->power) return false;
    if ((d->fields & WIZ_BRIGHTNESS) && d->brightness != o->brightness) return false;
    if ((d->fields & WIZ_RGB) && (d->red != o->red || d->green != o->green ||
        d->blue != o->blue || ((o->fields & WIZ_TEMPERATURE) && o->kelvin))) return false;
    if ((d->fields & WIZ_TEMPERATURE) && d->kelvin != o->kelvin) return false;
    if ((d->fields & (WIZ_RGB | WIZ_TEMPERATURE)) &&
        (o->fields & WIZ_SCENE) && o->scene != 0) return false;
    if ((d->fields & WIZ_SCENE) && d->scene != o->scene) return false;
    if ((d->fields & WIZ_SPEED) && d->speed != o->speed) return false;
    return true;
}

typedef struct { char *p; size_t size, used; bool failed; } writer_t;
static void append(writer_t *w, const char *format, ...)
{
    if (w->failed) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(w->p + w->used, w->size - w->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->size - w->used) w->failed = true;
    else w->used += (size_t)n;
}

wiz_error_t wiz_encode_request(wiz_method_t method, const wiz_patch_t *p,
                               const wiz_capabilities_t *caps,
                               const wiz_security_t *security,
                               wiz_wall_clock_t clock,
                               char *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!out || !capacity || !written || !security) return WIZ_ARGUMENT;
    out[0] = 0;
    const char *name = wiz_method_name(method);
    if (!name) return WIZ_UNSUPPORTED;
    if (method == WIZ_SET_PILOT) {
        wiz_error_t e = wiz_validate_patch(p, caps);
        if (e != WIZ_OK) return e;
    } else if (p) return WIZ_ARGUMENT;
    /* A provisioned key always signs, even if a caller has not yet made the
     * policy required. There is no retry-time unsigned downgrade. */
    bool signing = security->signing_required || security->key.present;
    if (signing && !security->key.present) return WIZ_KEY_REQUIRED;
    if (signing && (!clock.trusted || clock.unix_seconds < UINT64_C(1577836800) ||
                    clock.unix_seconds > UINT64_C(4102444800))) return WIZ_CLOCK_REQUIRED;
    char params[256] = {0};
    writer_t w = {params, sizeof(params), 0, false};
    append(&w, "{");
    bool comma = false;
#define FIELD(name_, format_, value_) do { \
    append(&w, "%s\"" name_ "\":" format_, comma ? "," : "", value_); \
    comma = true; \
} while (0)
    if (p) {
        if (p->fields & WIZ_POWER) FIELD("state", "%s", p->power ? "true" : "false");
        if (p->fields & WIZ_SPEED) FIELD("speed", "%u", (unsigned)p->speed);
        if (p->fields & WIZ_SCENE) FIELD("sceneId", "%u", (unsigned)p->scene);
        if (p->fields & WIZ_BRIGHTNESS) FIELD("dimming", "%u", (unsigned)p->brightness);
        if (p->fields & WIZ_RGB) {
            FIELD("r", "%u", (unsigned)p->red);
            FIELD("g", "%u", (unsigned)p->green);
            FIELD("b", "%u", (unsigned)p->blue);
        }
        if (p->fields & WIZ_TEMPERATURE) FIELD("temp", "%u", (unsigned)p->kelvin);
    }
    if (signing) FIELD("sigTs", "%llu", (unsigned long long)clock.unix_seconds);
    append(&w, "}");
#undef FIELD
    if (w.failed) return WIZ_TOO_LARGE;
    char hmac[45] = {0};
    if (signing) {
        wiz_error_t e = wiz_hmac_base64(&security->key, (const uint8_t *)params, w.used, hmac);
        if (e != WIZ_OK) return e;
    }
    writer_t result = {out, capacity, 0, false};
    append(&result, "{\"method\":\"%s\"", name);
    if (signing) append(&result, ",\"env\":\"pro\"");
    append(&result, ",\"params\":%s", params);
    if (signing) append(&result, ",\"hmac\":\"%s\"", hmac);
    append(&result, "}");
    if (result.failed || result.used > WIZ_DATAGRAM_MAX) {
        out[0] = 0;
        return WIZ_TOO_LARGE;
    }
    *written = result.used;
    return WIZ_OK;
}

/* Preflight bounds allocations before cJSON allocates. The upstream 1.38.0
 * configuration fixture needs 153 structural tokens, within the 192 cap. */
static wiz_error_t json_budget(const uint8_t *data, size_t size)
{
    bool quoted = false, escaped = false;
    unsigned depth = 0, budget = 0;
    for (size_t n = 0; n < size; ++n) {
        uint8_t c = data[n];
        if (!c) return WIZ_MALFORMED;
        if (quoted) {
            if (escaped) {
                if (c == 'u' && n + 4 < size && !memcmp(data + n + 1, "0000", 4)) return WIZ_MALFORMED;
                escaped = false;
            } else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            else if (c < 0x20) return WIZ_MALFORMED;
        } else if (c == '"') { quoted = true; ++budget; }
        else if (c == '{' || c == '[') { if (++depth > 6) return WIZ_TOO_LARGE; ++budget; }
        else if (c == '}' || c == ']') { if (!depth) return WIZ_MALFORMED; --depth; }
        else if (c == ',' || c == ':') ++budget;
        if (budget > 192) return WIZ_TOO_LARGE;
    }
    return !quoted && !escaped && depth == 0 ? WIZ_OK : WIZ_MALFORMED;
}

static bool unique_keys(const cJSON *node)
{
    if (cJSON_IsObject(node)) {
        for (const cJSON *a = node->child; a; a = a->next) {
            for (const cJSON *b = a->next; b; b = b->next)
                if (!strcmp(a->string, b->string)) return false;
        }
    }
    for (const cJSON *c = node->child; c; c = c->next)
        if (!unique_keys(c)) return false;
    return true;
}

static const cJSON *field(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static bool integer(const cJSON *item, int64_t low, int64_t high, int64_t *value)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < (double)low || item->valuedouble > (double)high ||
        trunc(item->valuedouble) != item->valuedouble) return false;
    *value = (int64_t)item->valuedouble;
    return true;
}

static bool identifier(const cJSON *item, char *out, size_t capacity)
{
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t length = bounded_length(item->valuestring, capacity);
    if (!length || length >= capacity) return false;
    for (size_t n = 0; n < length; ++n) {
        char c = item->valuestring[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    memcpy(out, item->valuestring, length + 1);
    return true;
}

static bool parse_state(const cJSON *r, wiz_patch_t *p)
{
    const cJSON *state = field(r, "state");
    if (!cJSON_IsBool(state)) return false;
    p->fields = WIZ_POWER;
    p->power = cJSON_IsTrue(state);
    int64_t n;
#define NUMBER(key_, member_, flag_, max_) do { \
    const cJSON *v = field(r, key_); \
    if (v) { if (!integer(v, 0, max_, &n)) return false; \
        p->member_ = n; p->fields |= flag_; } \
} while (0)
    NUMBER("dimming", brightness, WIZ_BRIGHTNESS, 100);
    NUMBER("temp", kelvin, WIZ_TEMPERATURE, 10000);
    if (p->kelvin != 0 && p->kelvin < 1000) return false;
    NUMBER("sceneId", scene, WIZ_SCENE, 1000);
    NUMBER("speed", speed, WIZ_SPEED, 200);
    const cJSON *red = field(r, "r"), *green = field(r, "g"), *blue = field(r, "b");
    if (red || green || blue) {
        if (!integer(red, 0, 255, &n)) return false;
        p->red = n;
        if (!integer(green, 0, 255, &n)) return false;
        p->green = n;
        if (!integer(blue, 0, 255, &n)) return false;
        p->blue = n;
        p->fields |= WIZ_RGB;
    }
#undef NUMBER
    return true;
}

static bool parse_range(const cJSON *r, const char *name, wiz_capabilities_t *caps)
{
    const cJSON *range = field(r, name);
    if (!range) return true;
    if (!cJSON_IsArray(range)) return false;
    int count = cJSON_GetArraySize(range);
    if (count < 2 || count > 4) return false;
    uint16_t low = 10000, high = 1000;
    for (int i = 0; i < count; ++i) {
        int64_t n;
        if (!integer(cJSON_GetArrayItem(range, i), 1000, 10000, &n)) return false;
        if (n < low) low = n;
        if (n > high) high = n;
    }
    caps->kelvin_min = low;
    caps->kelvin_max = high;
    return true;
}

wiz_error_t wiz_parse_reply(const uint8_t *data, size_t size,
                            wiz_method_t expected, const uint8_t expected_mac[6],
                            wiz_reply_t *out)
{
    if (!out) return WIZ_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!data || !expected_mac || !size || !wiz_method_name(expected)) return WIZ_ARGUMENT;
    if (size > WIZ_DATAGRAM_MAX) return WIZ_TOO_LARGE;
    wiz_error_t preflight = json_budget(data, size);
    if (preflight != WIZ_OK) return preflight;
    char input[WIZ_DATAGRAM_MAX + 1];
    memcpy(input, data, size);
    input[size] = 0;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(input, size + 1, &end, true);
    if (!root) return WIZ_MALFORMED;
    wiz_reply_t reply = {.method = expected};
    wiz_error_t error = WIZ_MALFORMED;
    if (!cJSON_IsObject(root) || !unique_keys(root)) goto done;
    const cJSON *method = field(root, "method");
    if (!cJSON_IsString(method)) goto done;
    if (strcmp(method->valuestring, wiz_method_name(expected))) {
        error = WIZ_WRONG_METHOD;
        goto done;
    }
    const cJSON *sig = field(root, "hmac");
    if (sig) {
        if (!cJSON_IsString(sig) || bounded_length(sig->valuestring, 45) != 44) goto done;
        reply.signature_present = true;
    }
    const cJSON *r = field(root, "result"), *e = field(root, "error");
    if ((r && e) || (!r && !e)) goto done;
    if (e) {
        int64_t code;
        if (!cJSON_IsObject(e) || !integer(field(e, "code"), INT32_MIN, INT32_MAX, &code)) goto done;
        reply.remote_error = true;
        reply.remote_error_code = code;
        error = WIZ_REJECTED;
        *out = reply;
        goto done;
    }
    if (!cJSON_IsObject(r)) goto done;
    const cJSON *mac = field(r, "mac");
    if (mac) {
        if (!cJSON_IsString(mac) || wiz_parse_mac(mac->valuestring, reply.mac) != WIZ_OK) goto done;
        if (memcmp(reply.mac, expected_mac, 6)) { error = WIZ_WRONG_IDENTITY; goto done; }
        reply.identity_present = true;
    }
    if ((expected == WIZ_GET_PILOT || expected == WIZ_GET_SYSTEM) && !reply.identity_present) goto done;
    if (expected == WIZ_SET_PILOT) {
        const cJSON *success = field(r, "success");
        if (!cJSON_IsBool(success)) goto done;
        reply.accepted = cJSON_IsTrue(success);
        error = reply.accepted ? WIZ_OK : WIZ_REJECTED;
    } else if (expected == WIZ_GET_PILOT) {
        if (!parse_state(r, &reply.state)) goto done;
        reply.capability_delta.evidence = WIZ_EVIDENCE_PILOT;
        error = WIZ_OK;
    } else if (expected == WIZ_GET_SYSTEM) {
        const cJSON *module = field(r, "moduleName"), *fw = field(r, "fwVersion");
        if (module && !identifier(module, reply.capability_delta.module, sizeof(reply.capability_delta.module))) goto done;
        if (fw && !identifier(fw, reply.capability_delta.firmware, sizeof(reply.capability_delta.firmware))) goto done;
        reply.capability_delta.evidence = WIZ_EVIDENCE_SYSTEM;
        error = WIZ_OK;
    } else if (expected == WIZ_GET_MODEL || expected == WIZ_GET_USER) {
        if (!parse_range(r, "whiteRange", &reply.capability_delta) ||
            !parse_range(r, "extRange", &reply.capability_delta) ||
            !parse_range(r, "cctRange", &reply.capability_delta)) goto done;
        const cJSON *minimum = field(r, "minDimLevel");
        int64_t n;
        if (minimum) {
            if (!integer(minimum, 1, 100, &n)) goto done;
            reply.capability_delta.brightness_min = n;
        }
        reply.capability_delta.evidence = expected == WIZ_GET_MODEL ? WIZ_EVIDENCE_MODEL : WIZ_EVIDENCE_USER;
        error = WIZ_OK;
    } else if (expected == WIZ_GET_POWER) {
        int64_t n;
        if (!integer(field(r, "pc"), 0, UINT32_MAX, &n)) goto done;
        reply.has_power_mw = true;
        reply.power_mw = n;
        error = WIZ_OK;
    }
    if (error == WIZ_OK || error == WIZ_REJECTED) *out = reply;
done:
    cJSON_Delete(root);
    return error;
}
