#include "wiz_driver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned assertions;
#define CHECK(x) do { ++assertions; if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); assert(x); } } while (0)

static const uint8_t mac[] = {0xa8,0xbb,0x50,0x06,0x03,0x3d};
static const wiz_wall_clock_t clock_ok = {1790441723, true};
static const char *pilot_on = "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":true,\"dimming\":70,\"r\":1,\"g\":2,\"b\":3,\"temp\":0,\"sceneId\":0}}";
static const char *pilot_off = "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":false}}";
static const char *ack = "{\"method\":\"setPilot\",\"result\":{\"success\":true}}";
static const char *system_rgb = "{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"a8bb5006033d\",\"moduleName\":\"ESP01_SHRGB_03\",\"fwVersion\":\"1.25.0\"}}";
static const char *model_rgb = "{\"method\":\"getModelConfig\",\"result\":{\"minDimLevel\":1,\"cctRange\":[2200,2700,4800,6500]}}";

static wiz_error_t parse(const char *json, wiz_method_t method, wiz_reply_t *r)
{
    return wiz_parse_reply((const uint8_t *)json, strlen(json), method, mac, r);
}

static wiz_capabilities_t rgb_caps(void)
{
    wiz_capabilities_t caps = {0};
    wiz_reply_t reply;
    CHECK(parse(system_rgb, WIZ_GET_SYSTEM, &reply) == WIZ_OK);
    wiz_capabilities_merge(&caps, &reply);
    CHECK(!(caps.features & WIZ_TEMPERATURE));
    CHECK(caps.brightness_min == 10);
    CHECK(parse(model_rgb, WIZ_GET_MODEL, &reply) == WIZ_OK);
    wiz_capabilities_merge(&caps, &reply);
    CHECK(caps.features & WIZ_TEMPERATURE);
    return caps;
}

static void test_keys_and_signing(void)
{
    uint8_t parsed[6] = {0};
    CHECK(wiz_parse_mac("a8bb5006033d", parsed) == WIZ_OK && !memcmp(parsed, mac, 6));
    CHECK(wiz_parse_mac("A8:BB:50:06:03:3D", parsed) == WIZ_OK && !memcmp(parsed, mac, 6));
    const char *bad_mac[] = {"", "a8-bb-50-06-03-3d", "a8bb5006033", "a8bb5006033dd", "ggbb5006033d", "000000000000", "ffffffffffff", "01bb5006033d", "a8:bb:50:06:03:3d "};
    for (unsigned i = 0; i < sizeof(bad_mac) / sizeof(bad_mac[0]); ++i)
        CHECK(wiz_parse_mac(bad_mac[i], parsed) == WIZ_ARGUMENT);
    wiz_security_t security = {.signing_required = true};
    CHECK(wiz_parse_key("00112233445566778899aabbccddeeff", &security.key) == WIZ_OK);
    char hmac[45];
    const char *params = "{\"state\":true,\"sigTs\":1790441723}";
    CHECK(wiz_hmac_base64(&security.key, (const uint8_t *)params, strlen(params), hmac) == WIZ_OK);
    CHECK(!strcmp(hmac, "vARMv4JpVsxYabcBovABd25ISBzYT8+71v0CUtpU1sg="));
    wiz_capabilities_t caps = rgb_caps();
    wiz_patch_t patch = {.fields = WIZ_POWER, .power = 1};
    char buffer[2049]; size_t length = 999;
    CHECK(wiz_encode_request(WIZ_SET_PILOT, &patch, &caps, &security, clock_ok, buffer, sizeof(buffer), &length) == WIZ_OK);
    CHECK(!strcmp(buffer, "{\"method\":\"setPilot\",\"env\":\"pro\",\"params\":{\"state\":true,\"sigTs\":1790441723},\"hmac\":\"vARMv4JpVsxYabcBovABd25ISBzYT8+71v0CUtpU1sg=\"}"));
    CHECK(length == strlen(buffer));
    CHECK(wiz_encode_request(WIZ_SET_PILOT, &patch, &caps, &security, (wiz_wall_clock_t){1790441723, false}, buffer, sizeof(buffer), &length) == WIZ_CLOCK_REQUIRED && length == 0);
    CHECK(wiz_encode_request(WIZ_GET_PILOT, NULL, NULL, &security, (wiz_wall_clock_t){1, true}, buffer, sizeof(buffer), &length) == WIZ_CLOCK_REQUIRED);
    CHECK(wiz_encode_request(WIZ_GET_PILOT, NULL, NULL, &security, clock_ok, buffer, 5, &length) == WIZ_TOO_LARGE && buffer[0] == 0 && length == 0);
    CHECK(wiz_encode_request((wiz_method_t)99, NULL, NULL, &security, clock_ok, buffer, sizeof(buffer), &length) == WIZ_UNSUPPORTED);
    const char *bad_keys[] = {"", "001122", "00112233445566778899aabbccddeefg", "00112233445566778899aabbccddeeff00", "00112233445566778899aabbccddeef ", "0x00112233445566778899aabbccddeeff"};
    for (unsigned i = 0; i < sizeof(bad_keys) / sizeof(bad_keys[0]); ++i) {
        CHECK(wiz_parse_key(bad_keys[i], &security.key) == WIZ_ARGUMENT);
        CHECK(!security.key.present);
    }
    CHECK(wiz_encode_request(WIZ_GET_PILOT, NULL, NULL, &security, clock_ok, buffer, sizeof(buffer), &length) == WIZ_KEY_REQUIRED);
    security.signing_required = false;
    CHECK(wiz_encode_request(WIZ_GET_PILOT, NULL, NULL, &security, (wiz_wall_clock_t){0}, buffer, sizeof(buffer), &length) == WIZ_OK);
    CHECK(!strcmp(buffer, "{\"method\":\"getPilot\",\"params\":{}}"));
    CHECK(wiz_parse_key("00112233445566778899aabbccddeeff", &security.key) == WIZ_OK);
    CHECK(wiz_encode_request(WIZ_GET_PILOT, NULL, NULL, &security, (wiz_wall_clock_t){0}, buffer, sizeof(buffer), &length) == WIZ_CLOCK_REQUIRED);
    wiz_key_clear(&security.key);
    CHECK(!security.key.present);
    for (unsigned i = 0; i < 16; ++i) CHECK(security.key.bytes[i] == 0);
}

static void test_capabilities_and_patches(void)
{
    wiz_capabilities_t caps = rgb_caps();
    CHECK(caps.brightness_min == 1 && caps.kelvin_min == 2200 && caps.kelvin_max == 6500);
    CHECK(caps.evidence & WIZ_EVIDENCE_CURATED_PROFILE);
    CHECK(wiz_scene_supported(&caps, 1) && wiz_scene_supported(&caps, 40));
    CHECK(!wiz_scene_supported(&caps, 201) && !wiz_scene_supported(&caps, 256) && !wiz_scene_supported(&caps, 1000));
    wiz_patch_t p = {.fields = WIZ_BRIGHTNESS, .brightness = 1};
    CHECK(wiz_validate_patch(&p, &caps) == WIZ_OK);
    p.brightness = 0; CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p.brightness = 101; CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p = (wiz_patch_t){.fields = WIZ_TEMPERATURE, .kelvin = 2200};
    CHECK(wiz_validate_patch(&p, &caps) == WIZ_OK);
    p.kelvin = 2199; CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p.kelvin = 6501; CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p = (wiz_patch_t){.fields = WIZ_RGB | WIZ_TEMPERATURE, .kelvin = 2700};
    CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p = (wiz_patch_t){.fields = WIZ_POWER | WIZ_RGB, .red = 255};
    CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p = (wiz_patch_t){.fields = WIZ_POWER}; CHECK(wiz_validate_patch(&p, &caps) == WIZ_OK);
    p.red = 1; CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p = (wiz_patch_t){.fields = WIZ_SCENE | WIZ_SPEED, .scene = 1, .speed = 10};
    CHECK(wiz_validate_patch(&p, &caps) == WIZ_OK);
    p.speed = 201; CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p.speed = 9; CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p = (wiz_patch_t){.fields = WIZ_SPEED, .speed = 100};
    CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    wiz_reply_t r;
    CHECK(parse("{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"a8bb5006033d\",\"moduleName\":\"FUTURE_RGB_MODEL\"}}", WIZ_GET_SYSTEM, &r) == WIZ_OK);
    wiz_capabilities_merge(&caps, &r);
    CHECK(caps.features == 0 && caps.kelvin_min == 0 && caps.scene_count == 0);
    CHECK(parse(pilot_on, WIZ_GET_PILOT, &r) == WIZ_OK);
    wiz_capabilities_merge(&caps, &r);
    CHECK(caps.features == (WIZ_POWER | WIZ_BRIGHTNESS));
    CHECK(caps.brightness_min == 10);
    p = (wiz_patch_t){.fields = WIZ_BRIGHTNESS, .brightness = 9};
    CHECK(wiz_validate_patch(&p, &caps) == WIZ_ARGUMENT);
    p.brightness = 10; CHECK(wiz_validate_patch(&p, &caps) == WIZ_OK);
    p = (wiz_patch_t){.fields = WIZ_RGB}; CHECK(wiz_validate_patch(&p, &caps) == WIZ_UNSUPPORTED);
    CHECK(parse("{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"a8bb5006033d\",\"moduleName\":\"ESP14_SHTW1C_01\"}}", WIZ_GET_SYSTEM, &r) == WIZ_OK);
    wiz_capabilities_merge(&caps, &r);
    CHECK(!(caps.features & WIZ_RGB) && wiz_scene_supported(&caps, 6) && !wiz_scene_supported(&caps, 1));
    CHECK(parse(model_rgb, WIZ_GET_MODEL, &r) == WIZ_OK);
    wiz_capabilities_merge(&caps, &r);
    CHECK(caps.features & WIZ_TEMPERATURE);
    CHECK(parse("{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"a8bb5006033d\",\"moduleName\":\"ESP14_SHTW1C_01\",\"fwVersion\":\"9.0.0\"}}", WIZ_GET_SYSTEM, &r) == WIZ_OK);
    wiz_capabilities_merge(&caps, &r);
    CHECK(parse("{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"a8bb5006033d\",\"moduleName\":\"ESP14_SHTW1C_01\",\"fwVersion\":\"9.0.1\"}}", WIZ_GET_SYSTEM, &r) == WIZ_OK);
    wiz_capabilities_merge(&caps, &r);
    CHECK(!(caps.features & WIZ_TEMPERATURE) && caps.kelvin_min == 0);
    CHECK(parse("{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"a8bb5006033d\",\"moduleName\":\"ESP10_SOCKET_06\"}}", WIZ_GET_SYSTEM, &r) == WIZ_OK);
    wiz_capabilities_merge(&caps, &r);
    CHECK(caps.features == WIZ_POWER && caps.scene_count == 0);
}

static void test_parser(void)
{
    wiz_reply_t r;
    CHECK(parse(pilot_on, WIZ_GET_PILOT, &r) == WIZ_OK && r.identity_present);
    CHECK(r.state.power && r.state.brightness == 70 && r.state.red == 1);
    wiz_patch_t desired = {.fields = WIZ_RGB | WIZ_BRIGHTNESS, .red = 1, .green = 2, .blue = 3, .brightness = 70};
    CHECK(wiz_state_matches(&desired, &r.state));
    r.state.scene = 1; CHECK(!wiz_state_matches(&desired, &r.state));
    r.state.scene = 0; r.state.kelvin = 2700; CHECK(!wiz_state_matches(&desired, &r.state));
    CHECK(parse(ack, WIZ_SET_PILOT, &r) == WIZ_OK && r.accepted && r.state.fields == 0);
    CHECK(!wiz_state_matches(&desired, &r.state));
    CHECK(parse(ack, WIZ_GET_PILOT, &r) == WIZ_WRONG_METHOD);
    CHECK(parse("{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033e\",\"state\":true}}", WIZ_GET_PILOT, &r) == WIZ_WRONG_IDENTITY);
    const char *bad[] = {
        "{", "{}", "null", "[]", "{\"method\":\"getPilot\",\"result\":null}",
        "{\"method\":\"getPilot\",\"result\":{\"state\":true}}",
        "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":1}}",
        "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":true,\"state\":false}}",
        "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":true,\"st\\u0061te\":false}}",
        "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\\u0000evil\",\"state\":true}}",
        "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":true,\"dimming\":101}}",
        "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":true,\"dimming\":1.5}}",
        "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":true,\"r\":1}}",
        "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":true},\"error\":{\"code\":1}}",
        "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":true}} trailing"
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK(parse(bad[i], WIZ_GET_PILOT, &r) == WIZ_MALFORMED && r.state.fields == 0);
    CHECK(parse("{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb5006033d\",\"state\":true,\"x\":[[[[[[]]]]]]}}", WIZ_GET_PILOT, &r) == WIZ_TOO_LARGE);
    CHECK(parse("{\"method\":\"getModelConfig\",\"result\":{\"minDimLevel\":0}}", WIZ_GET_MODEL, &r) == WIZ_MALFORMED);
    CHECK(parse("{\"method\":\"getModelConfig\",\"result\":{\"cctRange\":[2200,12000]}}", WIZ_GET_MODEL, &r) == WIZ_MALFORMED);
    CHECK(parse("{\"method\":\"setPilot\",\"error\":{\"code\":-99,\"message\":\"auth failed\"}}", WIZ_SET_PILOT, &r) == WIZ_REJECTED && r.remote_error);
    CHECK(parse("{\"method\":\"getPower\",\"result\":{\"pc\":7500}}", WIZ_GET_POWER, &r) == WIZ_OK && r.power_mw == 7500);
    CHECK(parse("{\"method\":\"setPilot\",\"env\":\"pro\",\"hmac\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=\",\"result\":{\"success\":true}}", WIZ_SET_PILOT, &r) == WIZ_OK && r.signature_present);
    char huge[2049]; memset(huge, ' ', sizeof(huge));
    CHECK(wiz_parse_reply((const uint8_t *)huge, sizeof(huge), WIZ_GET_MODEL, mac, &r) == WIZ_TOO_LARGE);
    /* Every truncation of a valid packet is rejected; no stale partial output. */
    for (size_t i = 1; i < strlen(pilot_on); ++i)
        CHECK(wiz_parse_reply((const uint8_t *)pilot_on, i, WIZ_GET_PILOT, mac, &r) != WIZ_OK && r.state.fields == 0);
    uint32_t seed = 7;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        size_t length = iteration % sizeof(huge);
        for (size_t n = 0; n < length; ++n) { seed = seed * 1664525u + 1013904223u; huge[n] = (char)(seed >> 24); }
        wiz_error_t e = wiz_parse_reply((const uint8_t *)huge, length, WIZ_GET_PILOT, mac, &r);
        CHECK(e != WIZ_OK);
    }
}

static void test_observed_esp25_model(void)
{
    /* Model body was sanitized from a physical read on 2026-11-16. The system
     * envelope below uses an existing synthetic test MAC, not the room bulb. */
    FILE *fixture = fopen(WIZ_MODEL_FIXTURE, "rb");
    CHECK(fixture != NULL);
    uint8_t bytes[WIZ_DATAGRAM_MAX + 1];
    size_t size = fread(bytes, 1, sizeof(bytes), fixture);
    CHECK(!ferror(fixture) && feof(fixture) && size > 0 && size <= WIZ_DATAGRAM_MAX);
    CHECK(fclose(fixture) == 0);
    wiz_reply_t reply;
    CHECK(wiz_parse_reply(bytes, size, WIZ_GET_MODEL, mac, &reply) == WIZ_OK);
    CHECK(!reply.identity_present && !reply.signature_present);
    CHECK(reply.capability_delta.evidence == WIZ_EVIDENCE_MODEL);
    CHECK(reply.capability_delta.kelvin_min == 2200 && reply.capability_delta.kelvin_max == 6500);
    CHECK(reply.capability_delta.brightness_min == 10);
    wiz_capabilities_t unknown = {0};
    wiz_capabilities_merge(&unknown, &reply);
    CHECK(unknown.features == 0); /* Model flags alone do not grant color modes. */
    wiz_capabilities_t caps = {0};
    wiz_reply_t system;
    CHECK(parse("{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"a8bb5006033d\",\"moduleName\":\"ESP25_SHRGB_01\",\"fwVersion\":\"1.38.0\"}}",
                WIZ_GET_SYSTEM, &system) == WIZ_OK);
    wiz_capabilities_merge(&caps, &system);
    CHECK(!(caps.features & WIZ_TEMPERATURE));
    wiz_capabilities_merge(&caps, &reply);
    CHECK(caps.features == (WIZ_POWER | WIZ_BRIGHTNESS | WIZ_RGB | WIZ_TEMPERATURE | WIZ_SCENE | WIZ_SPEED));
    CHECK(caps.evidence == (WIZ_EVIDENCE_SYSTEM | WIZ_EVIDENCE_MODEL | WIZ_EVIDENCE_CURATED_PROFILE));
    CHECK(caps.brightness_min == 10 && caps.kelvin_min == 2200 && caps.kelvin_max == 6500);
    CHECK(caps.scene_count == 37); /* Curated profile, not physically tested effects. */
    wiz_patch_t patch = {.fields = WIZ_BRIGHTNESS, .brightness = 9};
    CHECK(wiz_validate_patch(&patch, &caps) == WIZ_ARGUMENT);
    patch.brightness = 10; CHECK(wiz_validate_patch(&patch, &caps) == WIZ_OK);
    patch = (wiz_patch_t){.fields = WIZ_TEMPERATURE, .kelvin = 2200};
    CHECK(wiz_validate_patch(&patch, &caps) == WIZ_OK);
    patch.kelvin = 6500; CHECK(wiz_validate_patch(&patch, &caps) == WIZ_OK);
    patch.kelvin = 6501; CHECK(wiz_validate_patch(&patch, &caps) == WIZ_ARGUMENT);
    /* nowc/hasGradient do not silently enable zone or gradient control. */
    patch = (wiz_patch_t){.fields = WIZ_RGB | WIZ_TEMPERATURE, .red = 255, .kelvin = 2700};
    CHECK(wiz_validate_patch(&patch, &caps) == WIZ_ARGUMENT);
}

typedef struct {
    wiz_peer_t peer;
    bool authorized, disconnected;
    unsigned sends, opens, closes, checks, reads;
    char last_sent[2049];
    const char *queued[8];
    uint32_t source_ip;
    uint16_t source_port;
    unsigned queued_count, queued_offset;
    bool oversized;
    bool oversized_as_ok;
    uint64_t send_delay_ms, authorization_expires_ms;
    bool send_clock_back, omit_guard, revoke_before_send, fail_after_send;
} fake_t;

static wiz_error_t fake_resolve(void *ctx, const uint8_t wanted[6], wiz_peer_t *peer)
{
    fake_t *f = ctx;
    if (f->disconnected || memcmp(wanted, mac, 6)) return WIZ_NO_PEER;
    *peer = f->peer;
    return WIZ_OK;
}
static wiz_error_t fake_open(void *ctx, const wiz_peer_t *peer)
{
    fake_t *f = ctx; ++f->opens;
    CHECK(peer->ip == f->peer.ip); return WIZ_OK;
}
static wiz_error_t fake_send(void *ctx, const wiz_peer_t *peer, const uint8_t *data,
                              size_t size, wiz_send_guard_t *guard)
{
    fake_t *f = ctx;
    if (f->omit_guard) return WIZ_OK; /* Broken transport contract must fail. */
    if (f->revoke_before_send) f->authorized = false;
    uint64_t sample = guard->last_now_ms + f->send_delay_ms;
    if (f->send_clock_back) --sample;
    wiz_error_t e = wiz_send_guard_check(guard, sample);
    if (e != WIZ_OK) return e;
    ++f->sends; guard->transmitted = true;
    CHECK(peer->ip == f->peer.ip && size < sizeof(f->last_sent));
    memcpy(f->last_sent, data, size); f->last_sent[size] = 0;
    return f->fail_after_send ? WIZ_CANCELLED : WIZ_OK;
}
static wiz_error_t fake_receive(void *ctx, uint8_t *out, size_t capacity, size_t *size, uint32_t *ip, uint16_t *port)
{
    fake_t *f = ctx; ++f->reads;
    CHECK(capacity == WIZ_DATAGRAM_MAX + 1);
    if (f->queued_offset == f->queued_count && !f->oversized) return WIZ_IDLE;
    *ip = f->source_ip; *port = f->source_port;
    if (f->oversized) {
        f->oversized = false;
        memset(out, ' ', capacity); memcpy(out, pilot_on, strlen(pilot_on));
        *size = capacity;
        return f->oversized_as_ok ? WIZ_OK : WIZ_TOO_LARGE;
    }
    const char *packet = f->queued[f->queued_offset++];
    *size = strlen(packet); CHECK(*size <= capacity);
    memcpy(out, packet, *size); return WIZ_OK;
}
static void fake_close(void *ctx) { ++((fake_t *)ctx)->closes; }
static bool fake_authorize(void *ctx, uint8_t index, const uint8_t id[16], const wiz_patch_t *p, uint64_t now)
{
    fake_t *f = ctx; ++f->checks;
    CHECK(index == 0 && id[0] == 7 && p->fields == WIZ_POWER);
    return f->authorized && (!f->authorization_expires_ms || now < f->authorization_expires_ms);
}
static void setup(fake_t *f, wiz_driver_t *d, bool signed_requests)
{
    memset(f, 0, sizeof(*f));
    f->peer = (wiz_peer_t){.ap_ip = 0xc0a80401, .netmask = 0xffffff00, .ip = 0xc0a80402};
    memcpy(f->peer.mac, mac, 6);
    f->authorized = true; f->source_ip = f->peer.ip; f->source_port = WIZ_PORT;
    wiz_io_t io = {.context=f, .resolve=fake_resolve, .open=fake_open, .send=fake_send,
        .receive=fake_receive, .close=fake_close, .authorize=fake_authorize};
    CHECK(wiz_driver_init(d, &io) == WIZ_OK);
    wiz_security_t security = {.signing_required = signed_requests};
    if (signed_requests) CHECK(wiz_parse_key("00112233445566778899aabbccddeeff", &security.key) == WIZ_OK);
    CHECK(wiz_driver_enroll(d, 0, mac, &security) == WIZ_OK);
    d->bulbs[0].capabilities = rgb_caps();
}
static void start_write(wiz_driver_t *d)
{
    wiz_patch_t desired = {.fields=WIZ_POWER, .power=1}; uint8_t id[16] = {7};
    CHECK(wiz_driver_begin(d, 0, WIZ_SET_PILOT, &desired, id, 100, 2100) == WIZ_OK);
}
static void queue(fake_t *f, const char *packet)
{
    f->queued_offset = 0; f->queued_count = 1; f->queued[0] = packet;
}

static void test_driver(void)
{
    fake_t f; wiz_driver_t d; wiz_completion_t c;
    setup(&f, &d, true);
    CHECK(wiz_driver_enroll(&d, 8, mac, &d.bulbs[0].security) == WIZ_ARGUMENT);
    CHECK(wiz_driver_enroll(&d, 1, mac, &d.bulbs[0].security) == WIZ_ARGUMENT);
    CHECK(wiz_driver_enroll(&d, 0, d.bulbs[0].mac, &d.bulbs[0].security) == WIZ_OK);
    CHECK(d.bulbs[0].security.key.present && d.bulbs[0].security.key.bytes[15] == 0xff);
    CHECK(!memcmp(d.bulbs[0].mac, mac, 6));
    d.bulbs[0].capabilities = rgb_caps();
    start_write(&d);
    CHECK(wiz_driver_begin(&d, 0, WIZ_GET_PILOT, NULL, NULL, 100, 1100) == WIZ_BUSY);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY && f.sends == 1);
    CHECK(strstr(f.last_sent, "\"hmac\"") != NULL);
    queue(&f, ack);
    CHECK(wiz_driver_poll(&d, 120, clock_ok, &c) == WIZ_BUSY && f.sends == 2);
    CHECK(d.acknowledged && strstr(f.last_sent, "getPilot") != NULL);
    CHECK(d.active); /* Acknowledgement alone did not complete successfully. */
    queue(&f, pilot_on);
    CHECK(wiz_driver_poll(&d, 140, clock_ok, &c) == WIZ_OK && c.error == WIZ_OK);
    CHECK(c.write_sent && c.acknowledged && c.observed && c.matches && f.closes == 1);
    CHECK(wiz_driver_poll(&d, 150, clock_ok, &c) == WIZ_IDLE);

    setup(&f, &d, false); start_write(&d);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    queue(&f, ack); CHECK(wiz_driver_poll(&d, 120, clock_ok, &c) == WIZ_BUSY);
    queue(&f, pilot_off); CHECK(wiz_driver_poll(&d, 140, clock_ok, &c) == WIZ_BUSY);
    CHECK(wiz_driver_poll(&d, 420, clock_ok, &c) == WIZ_BUSY);
    queue(&f, pilot_off); CHECK(wiz_driver_poll(&d, 440, clock_ok, &c) == WIZ_BUSY);
    CHECK(wiz_driver_poll(&d, 720, clock_ok, &c) == WIZ_BUSY);
    queue(&f, pilot_off); CHECK(wiz_driver_poll(&d, 740, clock_ok, &c) == WIZ_OK);
    CHECK(c.error == WIZ_UNCONFIRMED && c.acknowledged && c.observed && !c.matches);
    CHECK(f.sends == 4); /* One write, three bounded readbacks. */

    setup(&f, &d, false); start_write(&d);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    queue(&f, ack); CHECK(wiz_driver_poll(&d, 120, clock_ok, &c) == WIZ_BUSY);
    queue(&f, pilot_off); CHECK(wiz_driver_poll(&d, 140, clock_ok, &c) == WIZ_BUSY);
    CHECK(wiz_driver_poll(&d, 420, clock_ok, &c) == WIZ_BUSY);
    queue(&f, pilot_on); CHECK(wiz_driver_poll(&d, 440, clock_ok, &c) == WIZ_OK);
    CHECK(c.error == WIZ_OK && c.matches && f.sends == 3);

    setup(&f, &d, false); start_write(&d);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    f.authorized = false;
    CHECK(wiz_driver_poll(&d, 400, clock_ok, &c) == WIZ_OK && c.error == WIZ_CANCELLED && f.sends == 1);
    CHECK(c.write_sent && !c.observed); /* Could have been applied; never pretend undo. */

    setup(&f, &d, false); start_write(&d);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    f.peer.ip++;
    CHECK(wiz_driver_poll(&d, 120, clock_ok, &c) == WIZ_OK && c.error == WIZ_PEER_CHANGED && f.sends == 1);

    setup(&f, &d, false); start_write(&d);
    for (unsigned t = 100; t <= 1600; t += 300) CHECK(wiz_driver_poll(&d, t, clock_ok, &c) == WIZ_BUSY);
    CHECK(f.sends == 6); /* Three absolute writes, then three reads, not six writes. */
    CHECK(strstr(f.last_sent, "getPilot") != NULL);
    CHECK(wiz_driver_poll(&d, 1900, clock_ok, &c) == WIZ_OK && c.error == WIZ_TIMEOUT);

    setup(&f, &d, true); start_write(&d);
    CHECK(wiz_driver_poll(&d, 100, (wiz_wall_clock_t){0}, &c) == WIZ_OK && c.error == WIZ_CLOCK_REQUIRED && f.sends == 0);
    setup(&f, &d, true); start_write(&d);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    queue(&f, "{\"method\":\"setPilot\",\"error\":{\"code\":-99}}");
    CHECK(wiz_driver_poll(&d, 120, clock_ok, &c) == WIZ_OK && c.error == WIZ_REJECTED && f.sends == 1);
    CHECK(strstr(f.last_sent, "\"hmac\"") != NULL);

    setup(&f, &d, false); start_write(&d);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    queue(&f, ack); f.source_ip++;
    CHECK(wiz_driver_poll(&d, 120, clock_ok, &c) == WIZ_BUSY && !d.acknowledged);
    f.source_ip = f.peer.ip; f.source_port = 1234; queue(&f, ack);
    CHECK(wiz_driver_poll(&d, 140, clock_ok, &c) == WIZ_BUSY && !d.acknowledged);
    f.source_port = WIZ_PORT; f.oversized = true;
    CHECK(wiz_driver_poll(&d, 150, clock_ok, &c) == WIZ_OK && c.error == WIZ_TOO_LARGE);

    setup(&f, &d, false); start_write(&d);
    CHECK(wiz_driver_poll(&d, 99, clock_ok, &c) == WIZ_OK && c.error == WIZ_CLOCK_ROLLBACK && f.sends == 0);
    setup(&f, &d, false); start_write(&d);
    CHECK(wiz_driver_poll(&d, 2100, clock_ok, &c) == WIZ_OK && c.error == WIZ_TIMEOUT && f.sends == 0);
    setup(&f, &d, false); start_write(&d); f.disconnected = true;
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_OK && c.error == WIZ_NO_PEER && f.sends == 0);

    setup(&f, &d, false);
    CHECK(wiz_driver_begin(&d, 0, WIZ_GET_SYSTEM, NULL, NULL, 100, 2100) == WIZ_OK);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    queue(&f, system_rgb);
    CHECK(wiz_driver_poll(&d, 120, clock_ok, &c) == WIZ_OK && c.error == WIZ_OK && !c.write_sent);
    CHECK(!strcmp(d.bulbs[0].capabilities.module, "ESP01_SHRGB_03"));
    CHECK(wiz_driver_remove(&d, 0) == WIZ_OK && !d.bulbs[0].enrolled);
    CHECK(wiz_driver_begin(&d, 0, WIZ_GET_PILOT, NULL, NULL, 150, 1000) == WIZ_NOT_ENROLLED);

    setup(&f, &d, false); start_write(&d);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    for (unsigned i = 0; i < 8; ++i) f.queued[i] = "{}";
    f.queued_count = 8;
    CHECK(wiz_driver_poll(&d, 120, clock_ok, &c) == WIZ_BUSY && f.queued_offset == 4);
    CHECK(wiz_driver_cancel(&d, &c) == WIZ_OK && c.error == WIZ_CANCELLED);

    CHECK(wiz_peer_allowed(&f.peer, mac));
    const uint32_t bad_ips[] = {0xc0a80400, 0xc0a804ff, 0xc0a80401, 0xc0a80502, 0x08080808, 0xe0000001};
    for (unsigned i = 0; i < sizeof(bad_ips) / sizeof(bad_ips[0]); ++i) {
        wiz_peer_t p = f.peer; p.ip = bad_ips[i]; CHECK(!wiz_peer_allowed(&p, mac));
    }
    wiz_peer_t p = f.peer; p.netmask = 0xfffffffd; CHECK(!wiz_peer_allowed(&p, mac));
    p = f.peer; p.mac[5]++; CHECK(!wiz_peer_allowed(&p, mac));
}

static void test_send_boundary(void)
{
    fake_t f; wiz_driver_t d; wiz_completion_t c;
    /* A poll one millisecond before expiry can spend its final millisecond in
     * DHCP work. Neither a command nor a standalone read may then be sent. */
    setup(&f, &d, false); start_write(&d); f.send_delay_ms = 1;
    CHECK(wiz_driver_poll(&d, 2099, clock_ok, &c) == WIZ_OK && c.error == WIZ_TIMEOUT);
    CHECK(f.sends == 0 && !c.write_sent && d.last_now_ms == 2100);
    setup(&f, &d, false);
    CHECK(wiz_driver_begin(&d, 0, WIZ_GET_PILOT, NULL, NULL, 100, 101) == WIZ_OK);
    f.send_delay_ms = 1;
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_OK && c.error == WIZ_TIMEOUT);
    CHECK(f.sends == 0 && d.last_now_ms == 101);

    /* Guest expiry/revocation at the final boundary is distinct from the
     * request deadline, and the authorization gets the actual fresh sample. */
    setup(&f, &d, false); start_write(&d);
    f.send_delay_ms = 2; f.authorization_expires_ms = 102;
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_OK && c.error == WIZ_CANCELLED);
    CHECK(f.sends == 0 && d.last_now_ms == 102 && !c.write_sent);
    setup(&f, &d, false); start_write(&d); f.revoke_before_send = true;
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_OK && c.error == WIZ_CANCELLED);
    CHECK(f.sends == 0 && !c.write_sent);
    setup(&f, &d, false); start_write(&d); f.send_clock_back = true;
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_OK && c.error == WIZ_CLOCK_ROLLBACK);
    CHECK(f.sends == 0 && d.last_now_ms == 100);
    setup(&f, &d, false); start_write(&d); f.omit_guard = true;
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_OK && c.error == WIZ_IO);
    CHECK(f.sends == 0 && !c.write_sent);

    /* Pacing starts at the actual send, without extending the deadline. */
    setup(&f, &d, false); start_write(&d); f.send_delay_ms = 50;
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    CHECK(d.last_now_ms == 150 && d.next_send_ms == 450 && d.deadline_ms == 2100);
    CHECK(wiz_driver_poll(&d, 400, clock_ok, &c) == WIZ_BUSY && f.sends == 1);
    CHECK(wiz_driver_poll(&d, 450, clock_ok, &c) == WIZ_BUSY && f.sends == 2);
    CHECK(d.last_now_ms == 500 && d.next_send_ms == 800);
    CHECK(wiz_driver_cancel(&d, &c) == WIZ_OK);

    /* An outer bookkeeping failure must not erase physical-send evidence. */
    setup(&f, &d, false); start_write(&d); f.fail_after_send = true;
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_OK && c.error == WIZ_CANCELLED);
    CHECK(f.sends == 1 && c.write_sent && !c.observed);
    setup(&f, &d, false);
    CHECK(wiz_driver_begin(&d, 0, WIZ_GET_PILOT, NULL, NULL, UINT64_MAX - 2, UINT64_MAX) == WIZ_OK);
    CHECK(wiz_driver_poll(&d, UINT64_MAX - 2, clock_ok, &c) == WIZ_BUSY);
    CHECK(d.next_send_ms == UINT64_MAX);
    CHECK(wiz_driver_poll(&d, UINT64_MAX, clock_ok, &c) == WIZ_OK && c.error == WIZ_TIMEOUT);

    wiz_send_guard_t guard = {.last_now_ms = 100, .deadline_ms = 200};
    CHECK(wiz_send_guard_check(NULL, 100) == WIZ_ARGUMENT);
    CHECK(wiz_send_guard_check(&guard, 99) == WIZ_CLOCK_ROLLBACK && !guard.checked);
    CHECK(wiz_send_guard_check(&guard, 100) == WIZ_OK && guard.checked);
    CHECK(wiz_send_guard_check(&guard, 101) == WIZ_ARGUMENT);
}

static void test_packet_reuse(void)
{
    fake_t f; wiz_driver_t d; wiz_completion_t c;
    setup(&f, &d, true); start_write(&d);
    CHECK(sizeof(d.packet) == WIZ_DATAGRAM_MAX + 1);
    CHECK((void *)d.packet.tx == (void *)d.packet.rx);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
    char original[WIZ_DATAGRAM_MAX + 1]; strcpy(original, f.last_sent);
    queue(&f, "{}");
    CHECK(wiz_driver_poll(&d, 400, clock_ok, &c) == WIZ_BUSY && f.sends == 2);
    CHECK(!strcmp(original, f.last_sent)); /* RX overwrote TX; signed retry re-encoded. */
    queue(&f, ack);
    CHECK(wiz_driver_poll(&d, 420, clock_ok, &c) == WIZ_BUSY);
    CHECK(strstr(f.last_sent, "getPilot") && strstr(f.last_sent, "hmac"));

    /* Exactly 2048 bytes remains legal even though the receive buffer has an
     * extra detection byte. The parser owns/copies the result before reuse. */
    char maximum[WIZ_DATAGRAM_MAX + 1]; memset(maximum, ' ', WIZ_DATAGRAM_MAX);
    memcpy(maximum, pilot_on, strlen(pilot_on)); maximum[WIZ_DATAGRAM_MAX] = 0;
    queue(&f, maximum);
    CHECK(wiz_driver_poll(&d, 440, clock_ok, &c) == WIZ_OK && c.error == WIZ_OK && c.matches);

    for(unsigned reports_ok = 0; reports_ok < 2; ++reports_ok){
        setup(&f, &d, false);
        CHECK(wiz_driver_begin(&d, 0, WIZ_GET_PILOT, NULL, NULL, 100, 2100) == WIZ_OK);
        CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY);
        f.oversized = true; f.oversized_as_ok = reports_ok != 0;
        CHECK(wiz_driver_poll(&d, 120, clock_ok, &c) == WIZ_OK && c.error == WIZ_TOO_LARGE);
        CHECK(!c.observed && !c.matches); /* Valid JSON prefix is never accepted. */
    }
    setup(&f, &d, true); start_write(&d);
    CHECK(wiz_driver_poll(&d, 100, clock_ok, &c) == WIZ_BUSY); strcpy(original, f.last_sent);
    f.oversized = true; ++f.source_ip;
    CHECK(wiz_driver_poll(&d, 400, clock_ok, &c) == WIZ_BUSY && f.sends == 2);
    CHECK(!strcmp(original, f.last_sent)); /* Ignored foreign packet cannot corrupt retry. */
    CHECK(wiz_driver_cancel(&d, &c) == WIZ_OK);
}

int main(void)
{
    test_keys_and_signing(); test_capabilities_and_patches(); test_parser(); test_observed_esp25_model(); test_driver(); test_send_boundary(); test_packet_reuse();
    printf("WiZ C checks: %u assertions passed; driver=%zu bytes; protocol/crypto only, no bulb IO\n", assertions, sizeof(wiz_driver_t));
    return 0;
}
