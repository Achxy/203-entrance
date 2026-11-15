#include "lighting_session_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t putvar(uint8_t *p, unsigned value)
{
    size_t n = 0;
    do { p[n++] = (uint8_t)((value & 127u) | (value > 127u ? 128u : 0u)); value >>= 7; } while (value);
    return n;
}
static size_t field(uint8_t *p, unsigned tag, const uint8_t *value, size_t size)
{
    size_t n = putvar(p, tag);
    n += putvar(p+n, (unsigned)size);
    memcpy(p+n, value, size);
    return n+size;
}
static size_t command(uint8_t *p, const char *username, unsigned step)
{
    uint8_t content[500], security[550], key[384];
    memset(key, 0x61, sizeof(key));
    size_t a = step ? field(content, 10, key, 64) :
                      field(content, 10, (const uint8_t *)username, strlen(username));
    if (!step) a += field(content+a, 18, key, sizeof(key));
    size_t b = 0;
    if (step) { security[b++] = 8; security[b++] = 2; }
    b += field(security+b, step ? 178u : 162u, content, a);
    p[0] = 16; p[1] = 2;
    return 2 + field(p+2, 98, security, b);
}

int main(void)
{
    uint8_t packet[1200], copy[1200]; ls_role_t role = LS_ROLE_NONE;
    size_t n = command(packet, LS_OWNER_USERNAME, 0);
    assert(ls_client_handshake(packet, n, 0, &role) && role == LS_ROLE_OWNER);
    for (size_t i = 0; i < n; ++i) assert(!ls_client_handshake(packet, i, 0, &role));
    memcpy(copy, packet, n); copy[1] = 1;
    assert(!ls_client_handshake(copy, n, 0, &role));
    memcpy(copy, packet, n); copy[n] = 16; copy[n+1] = 2;
    assert(!ls_client_handshake(copy, n+2, 0, &role));
    memcpy(copy, packet, n); copy[n] = 24; copy[n+1] = 1;
    assert(!ls_client_handshake(copy, n+2, 0, &role));
    assert(!ls_client_handshake(packet, n, 1, &role));
    assert(!ls_client_handshake(packet, n, 2, &role));
    n = command(packet, LS_GUEST_USERNAME, 0);
    assert(ls_client_handshake(packet, n, 0, &role) && role == LS_ROLE_GUEST);
    n = command(packet, "203-admin", 0);
    assert(!ls_client_handshake(packet, n, 0, &role));
    n = command(packet, LS_GUEST_USERNAME, 1);
    role = LS_ROLE_OWNER;
    assert(ls_client_handshake(packet, n, 1, &role) && role == LS_ROLE_OWNER);
    for (size_t i = 0; i < n; ++i) assert(!ls_client_handshake(packet, i, 1, &role));
    assert(!ls_client_handshake(NULL, n, 1, &role));
    assert(!ls_client_handshake(packet, 1025, 1, &role));
    uint8_t malformed[] = {0x90,0,2,98,0};
    assert(!ls_client_handshake(malformed, sizeof(malformed), 0, &role));

    uint8_t boot[8] = {0,1,2,3,4,5,6,7}, transcript[32], challenge[32];
    memset(transcript, 0x11, 32); memset(challenge, 0xab, 32);
    char context[LS_BINDING_CONTEXT_MAX];
    const char *expected = "203-lights-channel-v1\nentrance-c7f088\n0001020304050607\n"
        "1111111111111111111111111111111111111111111111111111111111111111\n"
        "abababababababababababababababababababababababababababababababab\nowner\n";
    assert(ls_binding_context(context, "entrance-c7f088", boot, transcript, challenge,
                               LS_ROLE_OWNER) == strlen(expected));
    assert(!strcmp(context, expected));
    assert(!ls_binding_context(context, "entrance\nowner", boot, transcript, challenge, LS_ROLE_OWNER));
    assert(!ls_binding_context(context, "entrance-c7f088", boot, transcript, challenge, LS_ROLE_NONE));
    uint8_t decoded[2];
    assert(ls_lower_hex("abcd", 4, decoded, 2) && decoded[0] == 0xab && decoded[1] == 0xcd);
    assert(!ls_lower_hex("Abcd", 4, decoded, 2));
    assert(!ls_lower_hex("abc", 3, decoded, 2));
    assert(!ls_lower_hex("ab g", 4, decoded, 2));

    /* Deterministic malformed packet corpus under ASan/UBSan, not a security
     * proof. It exercises bounded parsing independently of protobuf-c. */
    uint32_t random = 0x203c3u;
    for (unsigned round = 0; round < 50000; ++round) {
        random = random*1664525u + 1013904223u;
        size_t size = random % sizeof(packet);
        for (size_t i = 0; i < size; ++i) {
            random = random*1664525u + 1013904223u; packet[i] = (uint8_t)(random >> 24);
        }
        (void)ls_client_handshake(packet, size, round & 1u, &role);
    }
    puts("PASS Security2 packet bounds, transcript context and 50,000 malformed packets");
    return 0;
}
