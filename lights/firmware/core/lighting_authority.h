#ifndef LIGHTING_AUTHORITY_H
#define LIGHTING_AUTHORITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LG_MAX_LIGHTS 8u
#define LG_MAX_GUESTS 16u
#define LG_LEDGER_COUNT 32u
#define LG_OWNER_RESERVED 4u
#define LG_OWNER_SESSION UINT64_C(1)
#define LG_ALL_LIGHTS UINT8_C(255)
#define LG_MAX_COMMAND_TTL_MS UINT64_C(2000)
#define LG_DEDUPE_GRACE_MS UINT64_C(1000)
#define LG_MAX_GUEST_TTL_MS UINT64_C(86400000)
#define LG_MAX_OWNER_HOLD_MS UINT32_C(60000)
#define LG_POLICY_WIRE_SIZE 636u
#define LG_POLICY_VERSION 1u

typedef enum {
    LG_OK = 0, LG_DUPLICATE, LG_IDLE, LG_BAD_ARGUMENT, LG_NOT_READY,
    LG_UNAUTHORIZED, LG_REVOKED, LG_EXPIRED, LG_WRONG_BOOT,
    LG_POLICY_CHANGED, LG_STALE_SEQUENCE, LG_CONFLICT, LG_FRESH_READ_REQUIRED,
    LG_FROZEN, LG_OWNER_HOLD, LG_OVERLOADED, LG_ID_REUSE, LG_CLOCK_ERROR,
    LG_COUNTER_EXHAUSTED, LG_CORRUPT_POLICY, LG_STALE_STAGE, LG_NOT_FOUND
} lg_error_t;

typedef enum {
    LG_EMPTY = 0, LG_QUEUED, LG_SENT, LG_VERIFIED, LG_PARTIAL, LG_UNKNOWN,
    LG_REJECTED, LG_SUPERSEDED, LG_TIMED_OUT
} lg_result_t;

enum {
    LG_POWER = 1u, LG_BRIGHTNESS = 2u, LG_RGB = 4u,
    LG_TEMPERATURE = 8u, LG_SCENE = 16u, LG_SPEED = 32u
};

/* Absolute properties only. Absent properties must be zero. No toggle/delta. */
typedef struct {
    uint16_t fields;
    uint16_t kelvin;
    uint16_t scene;
    uint8_t power;
    uint8_t brightness;
    uint8_t red, green, blue;
    uint8_t speed;
} lg_intent_t;

/* TRUSTED ADAPTER INPUT: construct only after authenticating the transport.
 * Session 1 is the provisioned owner; all other roles come from stored grants.
 * This core neither authenticates credentials nor verifies cryptography. */
typedef struct {
    uint64_t session_id;
    uint64_t guest_epoch;
} lg_identity_t;

typedef struct {
    uint64_t id;
    uint64_t epoch;
    uint64_t boot_id;
    uint64_t expires_ms;
    uint8_t revoked;
} lg_guest_t;

typedef struct {
    uint64_t epoch;
    uint64_t guest_epoch;
    uint64_t next_guest_id;
    uint64_t clock_boot_id;
    uint64_t freeze_until_ms[LG_MAX_LIGHTS];
    lg_guest_t guests[LG_MAX_GUESTS];
} lg_policy_t;

/* Explicit copy: encode and durably commit before activate_after_commit(). */
typedef struct {
    lg_policy_t policy;
    uint64_t base_epoch;
    uint64_t boot_id;
    uint64_t now_ms;
    bool valid;
} lg_policy_stage_t;

typedef struct {
    uint8_t id[16];
    /* Adapter's canonical payload digest, checked alongside all actual fields.
     * The digest is not an authentication proof and is not computed here. */
    uint8_t digest[32];
    uint64_t boot_id;
    uint64_t policy_epoch;
    uint64_t sequence;
    uint64_t expires_ms;
    uint32_t expected_revision[LG_MAX_LIGHTS];
    uint32_t owner_hold_ms;
    lg_intent_t intent;
    uint8_t targets;
} lg_command_t;

typedef struct {
    lg_command_t command;
    uint64_t session_id;
    uint64_t guest_epoch;
    uint64_t retain_until_ms;
    lg_error_t error;
    lg_result_t result;
    uint8_t sent;
    uint8_t verified;
    uint8_t failed;
} lg_ledger_entry_t;

typedef struct {
    uint32_t revision;
    lg_intent_t desired;
    lg_intent_t observed;
    uint64_t observed_ms;
    bool observed_valid;
    bool pending;
    lg_result_t last_result;
} lg_light_t;

typedef struct {
    uint64_t session_id;
    uint64_t sequence;
    uint8_t needs_read;
} lg_sequence_t;

typedef struct {
    lg_policy_t policy;
    lg_ledger_entry_t ledger[LG_LEDGER_COUNT];
    lg_sequence_t guests[LG_MAX_GUESTS];
    lg_sequence_t owner;
    lg_light_t lights[LG_MAX_LIGHTS];
    uint64_t owner_hold_until_ms[LG_MAX_LIGHTS];
    uint64_t boot_id;
    uint64_t last_now_ms;
    uint8_t enabled;
    bool ready;
} lg_core_t;

typedef struct {
    uint8_t id[16];
    lg_result_t result;
    lg_error_t error;
    uint8_t sent, verified, failed;
} lg_receipt_t;

typedef struct {
    uint64_t boot_id;
    uint64_t policy_epoch;
    uint64_t guest_epoch;
    uint64_t last_sequence;
    uint8_t frozen;
    uint8_t owner_held;
    uint8_t enabled;
    lg_light_t lights[LG_MAX_LIGHTS];
} lg_snapshot_t;

/* Single-threaded authority: serialize calls in one worker, never GATT ISR.
 * All *_ms values are a trusted monotonic clock, not client wall time. */
void lg_policy_factory(lg_policy_t *policy, uint64_t boot_id);
lg_error_t lg_init(lg_core_t *core, const lg_policy_t *policy,
                   uint64_t boot_id, uint8_t enabled);
lg_error_t lg_policy_stage_begin(lg_core_t *core, lg_identity_t owner,
                                lg_policy_stage_t *stage, uint64_t now_ms);
lg_error_t lg_stage_admit_guest(lg_policy_stage_t *stage,
                               uint64_t proven_guest_epoch, uint64_t now_ms,
                               uint64_t ttl_ms, uint64_t *session_id);
/* Trusted fresh current-credential proof PLUS the existing unrevoked token is
 * required in the adapter. Boot-only renewal assigns a NEW ID in the SAME slot;
 * it cannot revive revoked, old-epoch, missing or same-boot expired sessions. */
lg_error_t lg_stage_renew_guest(lg_policy_stage_t *stage,
                               uint64_t old_session_id, uint64_t proven_guest_epoch,
                               uint64_t now_ms, uint64_t ttl_ms,
                               uint64_t *new_session_id);
lg_error_t lg_stage_revoke(lg_policy_stage_t *stage, uint64_t session_id);
lg_error_t lg_stage_rotate_guest_epoch(lg_policy_stage_t *stage);
lg_error_t lg_stage_freeze(lg_policy_stage_t *stage, uint8_t targets,
                          uint64_t now_ms, uint64_t duration_ms);
lg_error_t lg_policy_encode(const lg_policy_t *policy, uint8_t *out,
                           size_t capacity, size_t *written);
lg_error_t lg_policy_decode(lg_policy_t *policy, const uint8_t *data, size_t size);
/* Only call AFTER an atomic durable write succeeded. No NVS I/O is done here.
 * Serialize stage/write/activate, or compare-and-swap storage by base_epoch. */
lg_error_t lg_policy_activate_after_commit(lg_core_t *core,
                                          const lg_policy_stage_t *stage,
                                          uint64_t now_ms);
lg_error_t lg_clear_owner_holds(lg_core_t *core, lg_identity_t owner,
                               uint8_t targets, uint64_t now_ms);
/* Serialize validate -> durable registry commit -> apply in the gateway worker.
 * No live re-init: grant sequences and the command ledger must survive edits. */
lg_error_t lg_validate_enabled_change(lg_core_t *core, lg_identity_t owner,
                                      uint8_t enabled, uint64_t now_ms);
lg_error_t lg_set_enabled_after_commit(lg_core_t *core, lg_identity_t owner,
                                      uint8_t enabled, uint64_t now_ms);
lg_error_t lg_snapshot(lg_core_t *core, lg_identity_t identity, uint8_t targets,
                       uint64_t now_ms, lg_snapshot_t *snapshot);
lg_error_t lg_submit(lg_core_t *core, lg_identity_t identity,
                     const lg_command_t *command, uint64_t now_ms,
                     lg_receipt_t *receipt);
lg_error_t lg_command_status(lg_core_t *core, lg_identity_t identity,
                             const uint8_t id[16], uint64_t now_ms,
                             lg_receipt_t *receipt);
lg_error_t lg_next(lg_core_t *core, uint64_t now_ms, lg_receipt_t *receipt);
/* Recheck immediately before EACH physical send/retry, including group fanout.
 * This copies the intent without claiming that a UDP packet has been sent. */
lg_error_t lg_check_send(lg_core_t *core, const uint8_t id[16],
                        uint8_t light_index, uint64_t now_ms,
                        lg_intent_t *intent);
/* Mark a successful UDP socket send, in the same serialized worker directly
 * after lg_check_send + send. This does not prove delivery or observed state. */
lg_error_t lg_prepare_send(lg_core_t *core, const uint8_t id[16],
                           uint8_t light_index, uint64_t now_ms,
                           lg_intent_t *intent);
/* Completion requires actual supported readback, not merely UDP send success.
 * NULL,false also completes a pre-send failure without claiming a send. */
lg_error_t lg_complete_target(lg_core_t *core, const uint8_t id[16],
                              uint8_t light_index, const lg_intent_t *observed,
                              bool verified, uint64_t now_ms);
/* Fresh independent observation on an idle target: reject stale/busy queries
 * and advance revision when observed properties change. For active commands,
 * use correlated lg_complete_target(), never an unrelated periodic poll. */
lg_error_t lg_observe(lg_core_t *core, uint8_t light_index,
                     uint32_t expected_revision, const lg_intent_t *observed,
                     uint64_t now_ms);
lg_error_t lg_tick(lg_core_t *core, uint64_t now_ms);
const char *lg_error_name(lg_error_t error);

_Static_assert(sizeof(lg_core_t) <= 8192, "lighting authority exceeds 8 KiB");
_Static_assert(sizeof(lg_policy_stage_t) <= 1024, "policy staging exceeds 1 KiB");
_Static_assert(LG_POLICY_WIRE_SIZE <= 1024, "policy record exceeds 1 KiB");

#endif
