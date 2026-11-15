#include "lighting_session_wire.h"
#include <stdio.h>
#include <string.h>

typedef struct { const uint8_t *p; size_t n; } view_t;

static bool varint(view_t *v, uint32_t *out)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 5; ++i) {
        if (!v->n) return false;
        uint8_t b = *v->p++; --v->n;
        if (i == 4 && b > 15) return false;
        value |= (uint32_t)(b & 127) << (7 * i);
        if (!(b & 128)) {
            if (i && !(b & 127)) return false;
            *out = value;
            return true;
        }
    }
    return false;
}

static bool bytes(view_t *v, view_t *out)
{
    uint32_t count;
    if (!varint(v, &count) || count > v->n) return false;
    *out = (view_t){v->p, count};
    v->p += count; v->n -= count;
    return true;
}

static bool client_payload(view_t v, unsigned step, ls_role_t *role)
{
    unsigned seen = 0;
    ls_role_t candidate = LS_ROLE_NONE;
    while (v.n) {
        uint32_t tag;
        view_t field;
        if (!varint(&v, &tag) || (tag != 10 && tag != 18) ||
            (step == 1 && tag != 10) || !bytes(&v, &field)) return false;
        unsigned bit = tag == 10 ? 1 : 2;
        if (seen & bit) return false;
        seen |= bit;
        if (step == 1) {
            if (field.n != 64) return false;
        } else if (tag == 10) {
            if (field.n == sizeof(LS_OWNER_USERNAME) - 1 &&
                !memcmp(field.p, LS_OWNER_USERNAME, field.n)) candidate = LS_ROLE_OWNER;
            else if (field.n == sizeof(LS_GUEST_USERNAME) - 1 &&
                     !memcmp(field.p, LS_GUEST_USERNAME, field.n)) candidate = LS_ROLE_GUEST;
            else return false;
        } else if (field.n != 384) return false;
    }
    if (seen != (step ? 1u : 3u)) return false;
    if (!step) *role = candidate;
    return true;
}

static bool security_payload(view_t v, unsigned step, ls_role_t *role)
{
    unsigned seen = 0;
    uint32_t message = 0;
    view_t payload = {0};
    while (v.n) {
        uint32_t tag;
        if (!varint(&v, &tag)) return false;
        if (tag == 8) {
            if ((seen & 1) || !varint(&v, &message)) return false;
            seen |= 1;
        } else if (tag == (step ? 178u : 162u)) {
            if ((seen & 2) || !bytes(&v, &payload)) return false;
            seen |= 2;
        } else return false;
    }
    /* Proto3 omits the default command0 enum. Command1 must explicitly be2. */
    return (seen & 2) && message == (step ? 2u : 0u) &&
           (!step || (seen & 1)) && client_payload(payload, step, role);
}

bool ls_client_handshake(const uint8_t *data, size_t size, unsigned step,
                         ls_role_t *credential)
{
    if (!data || !credential || !size || size > LS_PACKET_MAX || step > 1) return false;
    view_t v = {data, size}, security = {0};
    unsigned seen = 0;
    uint32_t version = 0;
    while (v.n) {
        uint32_t tag;
        if (!varint(&v, &tag)) return false;
        if (tag == 16) {
            if ((seen & 1) || !varint(&v, &version)) return false;
            seen |= 1;
        } else if (tag == 98) {
            if ((seen & 2) || !bytes(&v, &security)) return false;
            seen |= 2;
        } else return false;
    }
    return seen == 3 && version == 2 && security_payload(security, step, credential);
}

static void hex(const uint8_t *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2*i] = digits[bytes[i] >> 4]; out[2*i+1] = digits[bytes[i] & 15];
    }
    out[2*n] = 0;
}

bool ls_lower_hex(const char *text, size_t size, uint8_t *out, size_t capacity)
{
    if (!text || !out || size != capacity * 2) return false;
    for (size_t i = 0; i < capacity; ++i) {
        unsigned a = (unsigned char)text[2*i], b = (unsigned char)text[2*i+1];
        if (!((a >= '0' && a <= '9') || (a >= 'a' && a <= 'f')) ||
            !((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f'))) return false;
        a = a <= '9' ? a - '0' : a - 'a' + 10;
        b = b <= '9' ? b - '0' : b - 'a' + 10;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}

size_t ls_binding_context(char out[LS_BINDING_CONTEXT_MAX], const char *gateway,
                          const uint8_t boot[8], const uint8_t transcript[32],
                          const uint8_t challenge[32], ls_role_t credential)
{
    if (!out || !gateway || !boot || !transcript || !challenge ||
        (credential != LS_ROLE_GUEST && credential != LS_ROLE_OWNER)) return 0;
    size_t n = strlen(gateway);
    if (!n || n > 48) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!((gateway[i] >= 'a' && gateway[i] <= 'z') ||
              (gateway[i] >= '0' && gateway[i] <= '9') || gateway[i] == '-')) return 0;
    char boot_hex[17], transcript_hex[65], challenge_hex[65];
    hex(boot, 8, boot_hex); hex(transcript, 32, transcript_hex); hex(challenge, 32, challenge_hex);
    int written = snprintf(out, LS_BINDING_CONTEXT_MAX,
                           "203-lights-channel-v1\n%s\n%s\n%s\n%s\n%s\n",
                           gateway, boot_hex, transcript_hex, challenge_hex,
                           credential == LS_ROLE_OWNER ? "owner" : "guest");
    return written > 0 && (size_t)written < LS_BINDING_CONTEXT_MAX ? (size_t)written : 0;
}
