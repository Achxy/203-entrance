#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define HUB_MAX 2048
#define HUB_HEADER 16
enum { HELLO=1, JOIN=2, OPEN=3, DATA=4, ACK=5, CLOSE=6, SCAN=7 };
static inline uint32_t hub_u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static inline void hub_put32(uint8_t *p, uint32_t v)
{ for (int i=0; i<4; i++) p[i]=(uint8_t)(v>>(8*i)); }
static inline uint32_t hub_crc(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int bit=0; bit<8; bit++) crc=(crc>>1) ^ (0xEDB88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
static inline size_t hub_encode(uint8_t *out, uint8_t type, uint32_t id,
                                const void *data, size_t n)
{
    if (n > HUB_MAX) return 0;
    memcpy(out,"203H",4); out[4]=1; out[5]=type; out[6]=n; out[7]=n>>8;
    hub_put32(out+8,id);
    if (n) memcpy(out+HUB_HEADER,data,n);
    hub_put32(out+12,hub_crc(hub_crc(0,out,12),out+HUB_HEADER,n));
    return HUB_HEADER+n;
}
