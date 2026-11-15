#ifndef LIGHTING_RUNTIME_QUEUE_H
#define LIGHTING_RUNTIME_QUEUE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LRQ_SLOTS 3u
#define LRQ_PAYLOAD 1024u
#define LRQ_RATE_TOTAL 8u
#define LRQ_RATE_GUEST 6u

typedef enum { LRQ_UNKNOWN=0, LRQ_GUEST, LRQ_OWNER } lrq_role_t;
typedef enum {
    LRQ_OK=0, LRQ_ARGUMENT, LRQ_BOOT, LRQ_EXPIRED, LRQ_REPLAY,
    LRQ_IN_FLIGHT, LRQ_CAPACITY, LRQ_RATE, LRQ_NOT_FOUND
} lrq_result_t;
typedef struct {
    uint8_t route[16], boot[8];
    uint32_t id;
    uint8_t operation;
    uint64_t lease_ms;
    uint32_t ttl_ms;
    const uint8_t *payload;
    size_t size;
} lrq_request_t;
typedef struct {
    uint8_t route[16];
    uint32_t id;
    uint8_t operation;
    uint64_t deadline_ms;
    uint16_t size;
    uint8_t payload[LRQ_PAYLOAD];
} lrq_work_t;
typedef struct {
    lrq_work_t work;
    uint64_t idle_deadline;
    uint32_t last_id;
    lrq_role_t role;
    bool used, queued, processing, closing;
} lrq_slot_t;
typedef struct {
    lrq_slot_t slots[LRQ_SLOTS];
    uint64_t rate_at[LRQ_RATE_TOTAL];
    lrq_role_t rate_role[LRQ_RATE_TOTAL];
    unsigned rate_cursor, rate_count, next_slot;
    uint8_t boot[8];
} lrq_t;

/* Pure single-owner queue. The runtime serializes these calls with its mutex;
 * no crypto, clocks, sockets, logging or heap allocation happens here. */
void lrq_init(lrq_t *queue, const uint8_t boot[8]);
lrq_result_t lrq_submit(lrq_t *queue, const lrq_request_t *request, uint64_t now_ms);
bool lrq_take(lrq_t *queue, lrq_work_t *work, uint64_t now_ms);
bool lrq_complete(lrq_t *queue, const uint8_t route[16], uint32_t id, uint64_t now_ms);
void lrq_close(lrq_t *queue, const uint8_t route[16]);
void lrq_close_all(lrq_t *queue);
void lrq_expire(lrq_t *queue, uint64_t now_ms);
bool lrq_take_closed(lrq_t *queue, uint8_t route[16]);
bool lrq_set_role(lrq_t *queue, const uint8_t route[16], lrq_role_t proven_role, uint64_t now_ms);
/* Count a BLE exchange against the same global sliding admission budget. */
bool lrq_admit_external(lrq_t *queue, lrq_role_t proven_role, uint64_t now_ms);

#endif
