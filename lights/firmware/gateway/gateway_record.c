#include "gateway_record.h"
#include <string.h>

void lgs_secure_zero(void *data, size_t size)
{
    volatile uint8_t *p = data;
    while (size--) *p++ = 0;
}

bool lgs_utf8_valid(const uint8_t *text,size_t n)
{
    if(!text)return false;
    for(size_t i=0;i<n;){
        uint8_t first=text[i++];if(first<128)continue;
        unsigned remaining;uint32_t value,minimum;
        if(first>=0xc2&&first<=0xdf){remaining=1;value=first&31u;minimum=0x80;}
        else if(first>=0xe0&&first<=0xef){remaining=2;value=first&15u;minimum=0x800;}
        else if(first>=0xf0&&first<=0xf4){remaining=3;value=first&7u;minimum=0x10000;}
        else return false;
        if(n-i<remaining)return false;
        for(unsigned j=0;j<remaining;++j){uint8_t next=text[i++];if((next&0xc0)!=0x80)return false;value=(value<<6)|(next&63u);}
        if(value<minimum||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return false;
    }
    return true;
}
bool lgs_name_valid(const char *name)
{
    if (!name) return false;
    size_t n = 0;
    while (n <= LGS_NAME_MAX && name[n]) ++n;
    if (!n || n > LGS_NAME_MAX || !lgs_utf8_valid((const uint8_t *)name,n)) return false;
    bool visible = false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127) return false;
        if (c != ' ') visible = true;
    }
    return visible;
}

static bool nonzero(const uint8_t *p, size_t n)
{
    uint8_t result = 0;
    for (size_t i = 0; i < n; ++i) result |= p[i];
    return result != 0;
}

bool lgs_record_valid(const lgs_record_t *r)
{
    if (!r || !r->generation) return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!nonzero(r->profiles[i].salt, 16) || !nonzero(r->profiles[i].verifier, 384)) return false;
    if (r->has_home_key && !nonzero(r->home_key, 16)) return false;
    uint8_t policy[LG_POLICY_WIRE_SIZE]; size_t policy_size = 0;
    if (lg_policy_encode(&r->policy, policy, sizeof(policy), &policy_size) != LG_OK ||
        policy_size != sizeof(policy)) return false;
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
        const lgs_bulb_t *b = &r->bulbs[i];
        if (!b->present) continue;
        if (!nonzero(b->mac, 6) || (b->mac[0] & 1u) || !lgs_name_valid(b->name) ||
            (!b->legacy_allowed && !r->has_home_key)) return false;
        for (unsigned j = 0; j < i; ++j)
            if (r->bulbs[j].present && !memcmp(r->bulbs[j].mac, b->mac, 6)) return false;
    }
    for (unsigned i = 0; i < LG_MAX_GUESTS; ++i) {
        const lgs_grant_t *g = &r->grants[i];
        if (!g->id) { if(r->policy.guests[i].id)return false; continue; }
        if (g->id < 2 || !nonzero(g->token_hash, 32) || !lgs_name_valid(g->name) ||
            g->id != r->policy.guests[i].id) return false;
        for (unsigned j = 0; j < i; ++j) if (r->grants[j].id == g->id) return false;
    }
    return true;
}

uint8_t lgs_enabled_mask(const lgs_record_t *r)
{
    uint8_t mask = 0;
    if (r) for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i)
        if (r->bulbs[i].present) mask |= (uint8_t)(1u << i);
    return mask;
}

static void put(uint8_t **p, uint64_t value, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) { *(*p)++ = (uint8_t)value; value >>= 8; }
}
static uint64_t get(const uint8_t **p, unsigned n)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i) value |= (uint64_t)*(*p)++ << (8u*i);
    return value;
}
static uint32_t crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool lgs_record_encode(const lgs_record_t *r, uint8_t out[LGS_RECORD_BYTES])
{
    if (!out || !lgs_record_valid(r)) return false;
    memset(out, 0, LGS_RECORD_BYTES);
    uint8_t *p = out;
    memcpy(p, "L203", 4); p += 4;
    put(&p, LGS_RECORD_VERSION, 4); put(&p, r->generation, 8);
    for (unsigned i = 0; i < 2; ++i) {
        memcpy(p, r->profiles[i].salt, 16); p += 16;
        memcpy(p, r->profiles[i].verifier, 384); p += 384;
    }
    *p++ = r->has_home_key ? 1 : 0;
    if (r->has_home_key) memcpy(p, r->home_key, 16);
    p += 16;
    size_t policy_size = 0;
    if (lg_policy_encode(&r->policy, p, LG_POLICY_WIRE_SIZE, &policy_size) != LG_OK) return false;
    p += policy_size;
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
        const lgs_bulb_t *b = &r->bulbs[i];
        *p++ = b->present ? 1 : 0; *p++ = b->present && b->legacy_allowed ? 1 : 0;
        if (b->present) { memcpy(p, b->mac, 6); memcpy(p+6, b->name, strlen(b->name)); }
        p += 6 + LGS_NAME_MAX + 1;
    }
    for (unsigned i = 0; i < LG_MAX_GUESTS; ++i) {
        const lgs_grant_t *g = &r->grants[i];
        put(&p, g->id, 8);
        if (g->id) { memcpy(p, g->token_hash, 32); memcpy(p+32, g->name, strlen(g->name)); }
        p += 32 + LGS_NAME_MAX + 1;
    }
    /* The tail is reserved and must stay zero. This format is architecture
     * independent; do not serialize C struct padding into persistent records. */
    if ((size_t)(p-out) > LGS_RECORD_BYTES-4) return false;
    p = out + LGS_RECORD_BYTES-4;
    put(&p, crc32(out, LGS_RECORD_BYTES-4), 4);
    return true;
}

bool lgs_record_decode(lgs_record_t *r, const uint8_t *data, size_t n)
{
    if (!r || !data || n != LGS_RECORD_BYTES || memcmp(data, "L203", 4)) return false;
    const uint8_t *check = data + n - 4;
    if (get(&check, 4) != crc32(data, n-4)) return false;
    lgs_record_t next = {0};
    const uint8_t *p = data + 4;
    if (get(&p, 4) != LGS_RECORD_VERSION) return false;
    next.generation = get(&p, 8);
    for (unsigned i = 0; i < 2; ++i) {
        memcpy(next.profiles[i].salt, p, 16); p += 16;
        memcpy(next.profiles[i].verifier, p, 384); p += 384;
    }
    if (*p > 1) return false;
    next.has_home_key = *p++ != 0; memcpy(next.home_key, p, 16); p += 16;
    if (lg_policy_decode(&next.policy, p, LG_POLICY_WIRE_SIZE) != LG_OK) return false;
    p += LG_POLICY_WIRE_SIZE;
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
        lgs_bulb_t *b = &next.bulbs[i];
        if (p[0] > 1 || p[1] > 1) return false;
        b->present = *p++ != 0; b->legacy_allowed = *p++ != 0;
        memcpy(b->mac, p, 6); p += 6;
        memcpy(b->name, p, LGS_NAME_MAX+1); p += LGS_NAME_MAX+1;
        if (b->name[LGS_NAME_MAX] != 0) return false;
    }
    for (unsigned i = 0; i < LG_MAX_GUESTS; ++i) {
        lgs_grant_t *g = &next.grants[i];
        g->id = get(&p, 8); memcpy(g->token_hash, p, 32); p += 32;
        memcpy(g->name, p, LGS_NAME_MAX+1); p += LGS_NAME_MAX+1;
        if (g->name[LGS_NAME_MAX] != 0) return false;
    }
    while (p < data+n-4) if (*p++) return false;
    if (!lgs_record_valid(&next)) { lgs_secure_zero(&next, sizeof(next)); return false; }
    *r = next; lgs_secure_zero(&next, sizeof(next));
    return true;
}
