#ifndef LIGHTING_RUNTIME_H
#define LIGHTING_RUNTIME_H

#include "gateway_record.h"
#include "lighting_runtime_queue.h"
#include "lighting_runtime_provision.h"
#include "lighting_local_time.h"
#include "esp_err.h"
#include "esp_netif.h"

typedef enum {
    LRT_PEAK_ACTIVATION=0, LRT_PEAK_BLE_START, LRT_PEAK_SRP, LRT_PEAK_PROVISION
} lrt_peak_t;
typedef enum { LRT_ROLE_OMIT=0, LRT_ROLE_GUEST, LRT_ROLE_OWNER, LRT_ROLE_CLOSED } lrt_reply_role_t;
typedef enum {
    LRT_FAILURE_NONE=0, LRT_FAILURE_WORKER_ADMISSION, LRT_FAILURE_WORKER_ALLOCATION,
    LRT_FAILURE_ACTIVATION_ADMISSION, LRT_FAILURE_CONTEXT_ALLOCATION,
    LRT_FAILURE_STORAGE_LOAD, LRT_FAILURE_PROVISION, LRT_FAILURE_RETAINED_HEAP,
    LRT_FAILURE_BLE_ADMISSION, LRT_FAILURE_BLE_START, LRT_FAILURE_SRP_ADMISSION,
    LRT_FAILURE_APPLICATION_ADMISSION
} lrt_failure_stage_t;
typedef enum {
    LRT_REASON_NONE=0, LRT_REASON_PEAK_BUSY, LRT_REASON_POLICY, LRT_REASON_FREE_HEAP,
    LRT_REASON_LARGEST_BLOCK, LRT_REASON_ALLOCATION, LRT_REASON_STORAGE, LRT_REASON_PLATFORM
} lrt_failure_reason_t;
typedef struct {
    char gateway_id[49];
    uint8_t boot[8];
    void *context;
    esp_netif_t *(*ap_netif)(void *context);
    bool (*sign_identity)(const char *message,char signature_hex[161],void *context);
    bool (*quiescing)(void *context);
    /* Mandatory nonblocking shared peak reservation. It must exclude competing
     * TLS handshakes, release crypto and OTA allocations until end_peak. */
    bool (*begin_peak)(lrt_peak_t peak,void *context);
    void (*end_peak)(lrt_peak_t peak,void *context);
    /* Optional additional policy gate; hard heap guards still apply. */
    bool (*resources_available)(lrt_peak_t peak,void *context);
    /* Trusted UTC and its actual last-sync reference/monotonic age. False
     * refuses signed WiZ/NTP replies; never fabricate a fresh reference. */
    bool (*wall_clock)(llt_clock_t *sample,void *context);
    void (*relay_reply)(const uint8_t route[16],uint32_t id,uint16_t status,
                        const uint8_t *payload,size_t size,lrt_reply_role_t role,
                        void *context);
    /* Optional idle/remote-revocation notice. Payload-free, no fake request ID.
     * If omitted, the next request still fails closed and closes the channel. */
    void (*relay_closed)(const uint8_t route[16],void *context);
} lrt_config_t;

typedef struct {
    bool prepared, configuration_present, provisioning, active, ble_running;
    bool quiesced, storage_healthy, resource_blocked, worker_running;
    bool quarantined, quarantine_retry_used;
    int reset_reason;
    lpp_phase_t provision_phase;
    uint32_t provision_generation;
    uint64_t provision_deadline_ms;
    bool provision_outcome_unknown, provision_poller_ready;
    esp_err_t provision_result, last_error;
    uint32_t completed, rejected;
    unsigned secure_sessions;
    uint32_t free_heap, largest_block;
    bool recovery_pending;
    uint32_t recovery_attempts, recovery_failures;
    uint64_t recovery_at_ms;
    lrt_failure_stage_t failure_stage;
    lrt_failure_reason_t failure_reason;
    uint64_t failure_at_ms;
    uint32_t failure_free_heap, failure_largest_block, required_free_heap, required_largest_block;
    llt_status_t local_time;
    bool worker_stack_sample_valid;
    uint32_t worker_stack_min_free_bytes;
} lrt_status_t;

/* Read-only boot inspection. No radio, UDP, NVS writes, heavy service context
 * or worker is started for an unprovisioned device. Call once after store_init. */
esp_err_t lighting_runtime_prepare(const lrt_config_t *config);
/* Call after the existing AP/network has started. No-op while unprovisioned. */
esp_err_t lighting_runtime_start(void);
void lighting_runtime_status(lrt_status_t *out);
/* Existing parent loop, even offline/disabled. No new task/timer for retry
 * scheduling. Admission uses nonblocking locks and preserves the heap guards. */
void lighting_runtime_poll_recovery(uint64_t now_ms);
/* Lightweight expiry only, from an existing health tick. Nonblocking; no NVS,
 * network, allocation or task creation. Expired pre-commit credentials are wiped. */
void lighting_runtime_housekeeping(uint64_t now_ms);
const char *lighting_runtime_failure_stage_name(lrt_failure_stage_t stage);
const char *lighting_runtime_failure_reason_name(lrt_failure_reason_t reason);

/* Existing authenticated management/USB adapter ONLY. Queues one immutable
 * profile pair for at most30 seconds before commit admission. Close management
 * TLS after acceptance. Poll the same boot/generation's phase/result; acceptance
 * is not durable success. All persistence/service mutation occurs on the worker. */
esp_err_t lighting_runtime_provision_trusted_admin(const lgs_profile_t profiles[2]);
/* Existing authenticated local management adapter ONLY. Releases abnormal-reset
 * quarantine once for this boot; it never changes credentials. ESP_OK accepts
 * that release, not successful activation. Memory refusal keeps normal bounded
 * recovery; a conflicting OTA/quiesce or unhealthy storage refuses the release. */
esp_err_t lighting_runtime_retry_trusted_admin(void);
lrq_result_t lighting_runtime_submit_relay(const lrq_request_t *request);
void lighting_runtime_close_relay(const uint8_t route[16]);
/* Parent TLS/WebSocket disconnected: close only its relay routes/crypto. BLE,
 * durable grants, policy and owner holds remain intact. */
void lighting_runtime_relay_disconnected(void);

/* Reversible maintenance: drops transport sessions, stops BLE/UDP, and retains
 * durable policy plus volatile owner holds. No irreversible BT memory release.
 * Quiesce returns only after the worker acknowledges shutdown; <=7000ms. */
bool lighting_runtime_quiesce(uint32_t timeout_ms);
void lighting_runtime_resume(void);

#endif
