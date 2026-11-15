#ifndef LIGHTING_BLE_FRAME_H
#define LIGHTING_BLE_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LB_SERVICE_UUID "89f5e330-0ba9-4d7d-b7e4-320303000001"
#define LB_RX_UUID "89f5e330-0ba9-4d7d-b7e4-320303000002"
#define LB_TX_UUID "89f5e330-0ba9-4d7d-b7e4-320303000003"
#define LB_HEADER_BYTES 12u
#define LB_REQUEST_MAX 1024u
#define LB_RESPONSE_MAX 1026u
#define LB_ATTRIBUTE_MAX 512u
#define LB_MESSAGE_TIMEOUT_MS UINT64_C(10000)
#define LB_START 1u
#define LB_END 2u

typedef enum { LB_IDENTITY = 0, LB_SESSION = 1, LB_DATA = 2, LB_CLOSE = 3 } lb_operation_t;
typedef enum {
    LB_MORE = 0, LB_COMPLETE, LB_BAD_FRAME, LB_BAD_ORDER, LB_BUSY, LB_EXPIRED
} lb_frame_result_t;
typedef enum { LB_IDLE = 0, LB_ASSEMBLING, LB_READY, LB_PROCESSING } lb_rx_phase_t;

typedef struct {
    uint8_t bytes[LB_REQUEST_MAX];
    uint64_t deadline_ms;
    uint32_t id, last_id;
    uint16_t size, used;
    uint8_t operation;
    lb_rx_phase_t phase;
} lb_receiver_t;

void lb_receiver_init(lb_receiver_t *receiver);
/* Clear a completed operation without permitting reuse of its request ID. */
void lb_receiver_release(lb_receiver_t *receiver);
lb_frame_result_t lb_receiver_feed(lb_receiver_t *receiver, const uint8_t *frame,
                                   size_t size, uint64_t now_ms);
bool lb_receiver_expired(const lb_receiver_t *receiver, uint64_t now_ms);

/* Status is prepended once to the complete response body, not each fragment.
 * Caller chooses capacity <= min(negotiated ATT MTU - 3,512). */
size_t lb_response_fragment(uint8_t *out, size_t capacity, uint8_t operation,
                            uint32_t id, const uint8_t *body, size_t total,
                            size_t offset, size_t *next_offset);

#endif
