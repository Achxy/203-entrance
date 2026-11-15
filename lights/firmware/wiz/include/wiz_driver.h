#ifndef WIZ_DRIVER_H
#define WIZ_DRIVER_H
#include "wiz_protocol.h"

#define WIZ_MAX_OPERATION_MS UINT64_C(2000)
#define WIZ_RETRY_MS UINT64_C(300)
#define WIZ_MAX_SENDS 3u

/* IPv4 integers are HOST order. resolve must find this enrolled MAC in BOTH
 * the associated AP station list and AP DHCP lease list on every send. */
typedef struct {
    uint32_t ap_ip, netmask, ip;
    uint8_t mac[6];
} wiz_peer_t;

/* Synchronous, stack-owned send fence. The transport must call check AFTER any
 * address/lease/socket work, immediately before the nonblocking send syscall.
 * No blocking work may follow a successful check. It must set transmitted only
 * when the complete datagram was accepted by the local socket. Neither flag
 * means that a bulb received or applied the request. Never retain this pointer. */
typedef struct {
    void *authorize_context;
    bool (*authorize)(void *, uint8_t, const uint8_t[16], const wiz_patch_t *, uint64_t);
    const uint8_t *command_id;
    const wiz_patch_t *desired;
    uint64_t last_now_ms, deadline_ms;
    uint8_t index;
    bool checked, transmitted;
} wiz_send_guard_t;

/* Records the fresh monotonic sample even on expiry/cancellation. A regressed
 * clock fails closed without moving the stored clock backwards. */
wiz_error_t wiz_send_guard_check(wiz_send_guard_t *guard, uint64_t now_ms);

typedef struct {
    void *context;
    wiz_error_t (*resolve)(void *, const uint8_t mac[6], wiz_peer_t *peer);
    wiz_error_t (*open)(void *, const wiz_peer_t *peer);
    wiz_error_t (*send)(void *, const wiz_peer_t *, const uint8_t *, size_t,
                        wiz_send_guard_t *);
    /* Nonblocking: capacity is WIZ_DATAGRAM_MAX+1, permitting direct receive
     * with one oversize-detection byte. IDLE when empty; TOO_LARGE when wire
     * length exceeds WIZ_DATAGRAM_MAX. Never parse a truncated packet. Return
     * actual source IP (host order) and port. IO must not retain this buffer. */
    wiz_error_t (*receive)(void *, uint8_t *, size_t capacity, size_t *size,
                          uint32_t *source_ip, uint16_t *source_port);
    void (*close)(void *);
    /* REQUIRED for writes, called just before EVERY write/readback send.
     * Recheck lg_check_send(command_id,index,now) and exact canonical patch.
     * Return false for cancellation/revocation/freeze/hold/deadline/stale work. */
    bool (*authorize)(void *, uint8_t index, const uint8_t command_id[16],
                      const wiz_patch_t *desired, uint64_t now_ms);
} wiz_io_t;

typedef struct {
    bool enrolled;
    uint8_t mac[6];
    wiz_security_t security;
    wiz_capabilities_t capabilities;
} wiz_bulb_t;

typedef struct {
    wiz_error_t error;
    uint8_t index;
    wiz_method_t requested_method;
    uint8_t command_id[16];
    bool write_sent, acknowledged, observed, matches;
    /* Always unverified local-network evidence: no response HMAC verification
     * is claimed, even if signature_present is true in the reply. */
    wiz_reply_t reply;
} wiz_completion_t;

typedef struct {
    wiz_io_t io;
    wiz_bulb_t bulbs[WIZ_MAX_BULBS];
    wiz_peer_t peer;
    wiz_patch_t desired;
    wiz_reply_t last_observation;
    uint8_t command_id[16];
    uint64_t last_now_ms, deadline_ms, next_send_ms;
    size_t tx_size;
    uint8_t index, sends;
    wiz_method_t requested_method, wire_method;
    bool active, opened, have_peer, write_sent, acknowledged, have_observation;
    /* Receives are fully parsed/copied before encoding; every retry encodes
     * anew. Synchronous IO never retains a packet pointer after returning. */
    union {
        char tx[WIZ_DATAGRAM_MAX + 1];
        uint8_t rx[WIZ_DATAGRAM_MAX + 1];
    } packet;
} wiz_driver_t;

bool wiz_peer_allowed(const wiz_peer_t *peer, const uint8_t enrolled_mac[6]);
wiz_error_t wiz_driver_init(wiz_driver_t *driver, const wiz_io_t *io);
/* Trusted owner service only; persist successful enrollment before exposing it.
 * No key/credential is copied into diagnostic strings. */
wiz_error_t wiz_driver_enroll(wiz_driver_t *driver, uint8_t index,
                              const uint8_t mac[6], const wiz_security_t *security);
wiz_error_t wiz_driver_remove(wiz_driver_t *driver, uint8_t index);
wiz_error_t wiz_driver_begin(wiz_driver_t *driver, uint8_t index,
                             wiz_method_t method, const wiz_patch_t *patch,
                             const uint8_t command_id[16], uint64_t now_ms,
                             uint64_t deadline_ms);
/* BUSY while pending, IDLE without work, OK when a completion was emitted.
 * Poll from one serialized worker (recommended every 20-50ms), never an ISR.
 * A terminal error lives in completion.error. No IO callback blocks for a reply.
 * last_now_ms may advance beyond this poll's input during a physical send;
 * service completions/authority calls must use at least that fresh sample. */
wiz_error_t wiz_driver_poll(wiz_driver_t *driver, uint64_t now_ms,
                            wiz_wall_clock_t wall_clock,
                            wiz_completion_t *completion);
wiz_error_t wiz_driver_cancel(wiz_driver_t *driver, wiz_completion_t *completion);

_Static_assert(sizeof(wiz_driver_t) <= 4096, "WiZ driver exceeds static 4KiB budget");
#endif
