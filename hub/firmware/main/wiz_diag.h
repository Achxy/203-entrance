#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define HD_MAX_TARGETS 8u
#define HD_PACKET_MAX 2048u
#define HD_WINDOW_MS UINT64_C(25000)
enum { HD_SYSTEM, HD_MODEL, HD_USER, HD_PILOT, HD_TIME, HD_METHOD_COUNT };
enum { HD_META = 0x89, HD_BODY = 0x8a, HD_DONE = 0x8b };
typedef struct { uint8_t mac[6]; uint32_t ip; bool conflict; } hd_target_t;
typedef struct {
    void *context;
    bool (*send)(void *, uint32_t host_ip, const char *, size_t);
    bool (*emit)(void *, uint8_t kind, uint32_t id, const void *, size_t);
} hd_io_t;
typedef struct {
    hd_io_t io;
    hd_target_t targets[HD_MAX_TARGETS];
    uint32_t local_ip, netmask, broadcast, request;
    uint64_t last_now, deadline, discover_until, query_until, next_send;
    uint32_t replies, ignored, oversized, conflicts, timeouts;
    uint8_t count, index, method, sends;
    bool active, discovery, answered;
} hd_state_t;
/* Host-order IPv4; RFC1918 private subnets only. No public/loopback/multicast. */
bool hd_network(uint32_t local_ip, uint32_t mask);
bool hd_peer(const hd_state_t *, uint32_t ip);
bool hd_mac(const char *, uint8_t out[6]);
const char *hd_request(unsigned method, size_t *size);
bool hd_start(hd_state_t *, const hd_io_t *, uint32_t request, uint32_t ip,
              uint32_t mask, const uint8_t *allowlist, size_t count, uint64_t now);
void hd_poll(hd_state_t *, uint64_t now);
void hd_receive(hd_state_t *, uint32_t source_ip, uint16_t port,
                const uint8_t *, size_t size, uint64_t now);
void hd_stop(hd_state_t *, const char *static_reason);
