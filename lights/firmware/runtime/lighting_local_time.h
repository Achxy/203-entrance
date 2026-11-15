#ifndef LIGHTING_LOCAL_TIME_H
#define LIGHTING_LOCAL_TIME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LLT_NTP_BYTES 48u
#define LLT_DNS_MAX 96u
#define LLT_PEERS 8u
#define LLT_RATE_MS 1000u

typedef struct llt_clock {
    int64_t seconds;
    uint32_t microseconds;
    bool trusted;
    int64_t reference_seconds;
    uint32_t reference_microseconds;
    uint64_t age_us;
} llt_clock_t;
typedef struct { uint8_t mac[6]; bool used; uint64_t last_ms; } llt_peer_rate_t;
typedef struct {
    llt_peer_rate_t peers[LLT_PEERS];
    uint64_t recent[LLT_PEERS], last_ms;
    uint8_t count;
    bool initialized;
} llt_rate_t;

typedef enum {
    LLT_OFF=0, LLT_WAIT_AP, LLT_SOCKET_BUDGET_REFUSED, LLT_DNS_CONFIGURATION_REFUSED,
    LLT_BIND_FAILED, LLT_WAIT_CLOCK, LLT_READY
} llt_state_t;
typedef struct {
    llt_state_t state;
    uint32_t dns_replies, ntp_replies, malformed, foreign_peer, rate_limited;
    uint32_t clock_refused, io_errors;
} llt_status_t;
const char *llt_state_name(llt_state_t state);

/* One response per MAC per second, at most eight total in any second. A peer
 * cannot consume the other seven peers' response allowance. MAC membership
 * must first be checked against live AP association and DHCP state. */
bool llt_rate_allow(llt_rate_t *rate,const uint8_t mac[6],uint64_t now_ms);
bool llt_ntp_request(const uint8_t *request,size_t size);
/* True synchronization reference, exact monotonic-derived age, at most24h.
 * Uncertainty is an engineering allowance, not an upstream accuracy proof. */
bool llt_clock_valid(llt_clock_t clock);
/* Coarse trusted samples; never claim subsecond precision from integer UTC.
 * Era is implicit on the wire: seconds wrap modulo2^32 at2036. The all-zero
 * instant is refused because NTP reserves it for unknown time. */
bool llt_ntp_reply(const uint8_t *request,size_t size,llt_clock_t received,
                   llt_clock_t transmitted,uint8_t reply[LLT_NTP_BYTES]);
/* No forwarding/wildcard. Exact ntp.wiz.world A -> AP; AAAA -> NODATA;
 * other names -> NXDOMAIN. One uncompressed IN question, no EDNS/additional
 * records. Valid A replies add16 bytes; every reply is <=96 bytes. */
size_t llt_dns_reply(const uint8_t *request,size_t size,uint32_t ap_ipv4,
                      uint8_t reply[LLT_DNS_MAX]);
#endif
