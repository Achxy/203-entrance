#include "lighting_ble_frame.h"
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}
static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}

void lb_receiver_init(lb_receiver_t *r) { if (r) memset(r, 0, sizeof(*r)); }
void lb_receiver_release(lb_receiver_t *r)
{
    if (!r) return;
    uint32_t last = r->last_id;
    memset(r, 0, sizeof(*r));
    r->last_id = last;
}
bool lb_receiver_expired(const lb_receiver_t *r, uint64_t now)
{
    return r && r->phase != LB_IDLE && now >= r->deadline_ms;
}

lb_frame_result_t lb_receiver_feed(lb_receiver_t *r, const uint8_t *p, size_t n, uint64_t now)
{
    if (!r || !p || n < LB_HEADER_BYTES || n > LB_ATTRIBUTE_MAX || p[0] != 1 ||
        p[1] > LB_CLOSE || p[2] > (LB_START | LB_END) || p[3] != 0) return LB_BAD_FRAME;
    if (lb_receiver_expired(r, now)) return LB_EXPIRED;
    if (r->phase == LB_READY || r->phase == LB_PROCESSING) return LB_BUSY;
    uint32_t id = u32(p + 4);
    uint16_t total = u16(p + 8), offset = u16(p + 10);
    size_t payload = n - LB_HEADER_BYTES;
    bool first = (p[2] & LB_START) != 0, last = (p[2] & LB_END) != 0;
    if (!id || total > LB_REQUEST_MAX || (p[1] == LB_CLOSE ? total != 0 : total == 0) ||
        offset > total || payload > (size_t)(total - offset) ||
        (!payload && total) || (last != (offset + payload == total))) return LB_BAD_FRAME;
    if (r->phase == LB_IDLE) {
        if (!first || offset || id <= r->last_id || (total == 0 && !last)) return LB_BAD_ORDER;
        if (now > UINT64_MAX - LB_MESSAGE_TIMEOUT_MS) return LB_EXPIRED;
        r->id = r->last_id = id;
        r->size = total;
        r->operation = p[1];
        r->deadline_ms = now + LB_MESSAGE_TIMEOUT_MS;
        r->phase = LB_ASSEMBLING;
    } else if (first || id != r->id || p[1] != r->operation || total != r->size) return LB_BAD_ORDER;
    if (offset != r->used) return LB_BAD_ORDER;
    if (payload) memcpy(r->bytes + offset, p + LB_HEADER_BYTES, payload);
    r->used = (uint16_t)(r->used + payload);
    if (last) { r->phase = LB_READY; return LB_COMPLETE; }
    return LB_MORE;
}

size_t lb_response_fragment(uint8_t *out, size_t cap, uint8_t operation, uint32_t id,
                            const uint8_t *body, size_t total, size_t offset, size_t *next)
{
    if (!out || !body || !next || operation > LB_CLOSE || !id || total < 2 ||
        total > LB_RESPONSE_MAX || offset >= total || cap <= LB_HEADER_BYTES ||
        cap > LB_ATTRIBUTE_MAX) return 0;
    size_t count = total - offset;
    if (count > cap - LB_HEADER_BYTES) count = cap - LB_HEADER_BYTES;
    out[0] = 1; out[1] = (uint8_t)(operation | 0x80);
    out[2] = (uint8_t)((offset == 0 ? LB_START : 0) | (offset + count == total ? LB_END : 0));
    out[3] = 0;
    put32(out + 4, id); put16(out + 8, (uint16_t)total); put16(out + 10, (uint16_t)offset);
    memcpy(out + LB_HEADER_BYTES, body + offset, count);
    *next = offset + count;
    return LB_HEADER_BYTES + count;
}
