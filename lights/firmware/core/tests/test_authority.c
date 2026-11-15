#include "lighting_authority.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define EQ(a,b) CHECK((a) == (b))

typedef struct {
    lg_core_t core;
    lg_identity_t guest;
    uint64_t now;
    uint32_t id_counter;
} fixture_t;

static const lg_identity_t owner = {LG_OWNER_SESSION, 0};

static void commit(fixture_t *f, const lg_policy_stage_t *s) {
    uint8_t record[LG_POLICY_WIRE_SIZE];
    size_t written = 0;
    lg_policy_t decoded;
    EQ(lg_policy_encode(&s->policy, record, sizeof(record), &written), LG_OK);
    EQ(written, sizeof(record));
    EQ(lg_policy_decode(&decoded, record, sizeof(record)), LG_OK);
    EQ(decoded.epoch, s->policy.epoch);
    /* The test models a successful atomic durable write/readback. */
    EQ(lg_policy_activate_after_commit(&f->core, s, ++f->now), LG_OK);
}

static void setup(fixture_t *f) {
    memset(f, 0, sizeof(*f));
    f->now = 100;
    lg_policy_t policy;
    lg_policy_factory(&policy, 42);
    EQ(lg_init(&f->core, &policy, 42, 31), LG_OK);
    lg_policy_stage_t s;
    EQ(lg_policy_stage_begin(&f->core, owner, &s, ++f->now), LG_OK);
    EQ(lg_stage_admit_guest(&s, 1, f->now, 60000, &f->guest.session_id), LG_OK);
    f->guest.guest_epoch = 1;
    commit(f, &s);
}

static lg_snapshot_t read_state(fixture_t *f, lg_identity_t id) {
    lg_snapshot_t snapshot;
    EQ(lg_snapshot(&f->core, id, 31, ++f->now, &snapshot), LG_OK);
    return snapshot;
}

static void new_id(fixture_t *f, lg_command_t *cmd) {
    uint32_t id = ++f->id_counter;
    memset(cmd->id, 0, sizeof(cmd->id));
    memcpy(cmd->id, &id, sizeof(id));
    memset(cmd->digest, (int)(id % 255u + 1u), sizeof(cmd->digest));
}

static lg_command_t command(fixture_t *f, lg_identity_t id, uint8_t targets, uint64_t sequence) {
    lg_snapshot_t snapshot = read_state(f, id);
    lg_command_t cmd = {0};
    new_id(f, &cmd);
    cmd.boot_id = snapshot.boot_id;
    cmd.policy_epoch = snapshot.policy_epoch;
    cmd.sequence = sequence;
    cmd.expires_ms = f->now + 1000;
    cmd.targets = targets;
    cmd.intent = (lg_intent_t){.fields = LG_POWER | LG_BRIGHTNESS, .power = 1, .brightness = 55};
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i)
        if (targets & (1u << i)) cmd.expected_revision[i] = snapshot.lights[i].revision;
    return cmd;
}

static void duplicate_and_ordering(void) {
    fixture_t f; setup(&f);
    lg_receipt_t receipt;
    lg_command_t a = command(&f, f.guest, 1, 2);
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_QUEUED);
    CHECK(f.core.lights[0].pending);
    EQ(f.core.lights[0].last_result, LG_QUEUED);
    uint32_t revision = f.core.lights[0].revision;
    /* Same authenticated grant arrives over the other transport. */
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_DUPLICATE);
    EQ(f.core.lights[0].revision, revision);
    EQ(read_state(&f, f.guest).last_sequence, 2);
    lg_command_t altered = a;
    altered.digest[0] ^= 1;
    EQ(lg_submit(&f.core, f.guest, &altered, ++f.now, &receipt), LG_ID_REUSE);
    altered = a; altered.intent.brightness = 56;
    EQ(lg_submit(&f.core, f.guest, &altered, ++f.now, &receipt), LG_ID_REUSE);
    EQ(lg_submit(&f.core, owner, &a, ++f.now, &receipt), LG_ID_REUSE);
    lg_command_t older = command(&f, f.guest, 2, 1);
    EQ(lg_submit(&f.core, f.guest, &older, ++f.now, &receipt), LG_STALE_SEQUENCE);
    lg_command_t newer = command(&f, f.guest, 1, 3);
    EQ(lg_submit(&f.core, f.guest, &newer, ++f.now, &receipt), LG_OK);
    EQ(lg_command_status(&f.core, f.guest, a.id, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_SUPERSEDED);
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_DUPLICATE);
    EQ(receipt.result, LG_SUPERSEDED);
}

static void conflicts_require_reads(void) {
    fixture_t f; setup(&f);
    lg_receipt_t receipt;
    lg_command_t a = command(&f, f.guest, 1, 1);
    a.expected_revision[0]++;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_CONFLICT);
    new_id(&f, &a); a.expected_revision[0] = f.core.lights[0].revision;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_FRESH_READ_REQUIRED);
    (void)read_state(&f, f.guest);
    new_id(&f, &a);
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_OK);
    lg_command_t stale = command(&f, f.guest, 2, 2);
    stale.policy_epoch--;
    EQ(lg_submit(&f.core, f.guest, &stale, ++f.now, &receipt), LG_POLICY_CHANGED);
    new_id(&f, &stale); stale.policy_epoch = f.core.policy.epoch;
    EQ(lg_submit(&f.core, f.guest, &stale, ++f.now, &receipt), LG_FRESH_READ_REQUIRED);
}

static void policy_staging_and_freezes(void) {
    fixture_t f; setup(&f);
    lg_policy_stage_t s, stale;
    EQ(lg_policy_stage_begin(&f.core, f.guest, &s, ++f.now), LG_UNAUTHORIZED);
    CHECK(!s.valid);
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_freeze(&s, 4, f.now, UINT64_MAX), LG_OK);
    stale = s;
    /* Failed persistence leaves active policy and behavior unchanged. */
    lg_snapshot_t before = read_state(&f, f.guest);
    EQ(before.frozen, 0);
    lg_receipt_t receipt;
    lg_command_t pending = command(&f, f.guest, 4, 1);
    EQ(lg_submit(&f.core, f.guest, &pending, ++f.now, &receipt), LG_OK);
    commit(&f, &s);
    lg_intent_t out;
    EQ(lg_prepare_send(&f.core, pending.id, 2, ++f.now, &out), LG_POLICY_CHANGED);
    EQ(lg_policy_activate_after_commit(&f.core, &stale, ++f.now), LG_STALE_STAGE);
    lg_command_t group = command(&f, f.guest, 5, 2);
    uint32_t revision = f.core.lights[0].revision;
    EQ(lg_submit(&f.core, f.guest, &group, ++f.now, &receipt), LG_FROZEN);
    EQ(f.core.lights[0].revision, revision); /* group admission is all-or-nothing */
    lg_command_t other = command(&f, f.guest, 2, 3);
    EQ(lg_submit(&f.core, f.guest, &other, ++f.now, &receipt), LG_OK);
    lg_command_t admin = command(&f, owner, 4, 1);
    EQ(lg_submit(&f.core, owner, &admin, ++f.now, &receipt), LG_OK);
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_freeze(&s, 31, f.now, 50), LG_OK);
    commit(&f, &s);
    EQ(read_state(&f, f.guest).frozen, 31);
    f.now += 100;
    EQ(read_state(&f, f.guest).frozen, 0);
}

static void owner_priority_and_holds(void) {
    fixture_t f; setup(&f);
    lg_receipt_t receipt;
    lg_command_t guest = command(&f, f.guest, 1, 1);
    EQ(lg_submit(&f.core, f.guest, &guest, ++f.now, &receipt), LG_OK);
    lg_command_t admin = command(&f, owner, 2, 1);
    admin.owner_hold_ms = 1000;
    EQ(lg_submit(&f.core, owner, &admin, ++f.now, &receipt), LG_OK);
    EQ(lg_next(&f.core, ++f.now, &receipt), LG_OK);
    CHECK(!memcmp(receipt.id, admin.id, 16));
    lg_command_t override = command(&f, owner, 1, 2);
    override.owner_hold_ms = 1000;
    EQ(lg_submit(&f.core, owner, &override, ++f.now, &receipt), LG_OK);
    lg_intent_t out;
    EQ(lg_prepare_send(&f.core, guest.id, 0, ++f.now, &out), LG_CONFLICT);
    lg_command_t denied = command(&f, f.guest, 1, 2);
    EQ(lg_submit(&f.core, f.guest, &denied, ++f.now, &receipt), LG_OWNER_HOLD);
    denied = command(&f, f.guest, 4, 3); denied.owner_hold_ms = 1;
    EQ(lg_submit(&f.core, f.guest, &denied, ++f.now, &receipt), LG_BAD_ARGUMENT);
    EQ(lg_clear_owner_holds(&f.core, f.guest, 1, ++f.now), LG_UNAUTHORIZED);
    EQ(lg_clear_owner_holds(&f.core, owner, 1, ++f.now), LG_OK);
    denied = command(&f, f.guest, 1, 4);
    EQ(lg_submit(&f.core, f.guest, &denied, ++f.now, &receipt), LG_OK);
    /* Heartbeats/reads cannot extend the hold on target 1. */
    f.now += 1100;
    EQ(read_state(&f, f.guest).owner_held, 0);
}

static void revoke_rotation_and_no_id_reuse(void) {
    fixture_t f; setup(&f);
    lg_identity_t original = f.guest;
    lg_policy_stage_t s;
    lg_receipt_t receipt;
    lg_command_t cmd = command(&f, f.guest, 1, 1);
    EQ(lg_submit(&f.core, f.guest, &cmd, ++f.now, &receipt), LG_OK);
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_revoke(&s, original.session_id), LG_OK);
    uint64_t replacement = 0;
    EQ(lg_stage_admit_guest(&s, 1, f.now, 60000, &replacement), LG_OK);
    CHECK(replacement > original.session_id);
    commit(&f, &s);
    EQ(lg_submit(&f.core, original, &cmd, ++f.now, &receipt), LG_REVOKED);
    lg_snapshot_t snapshot;
    EQ(lg_snapshot(&f.core, original, 1, ++f.now, &snapshot), LG_REVOKED);
    f.guest.session_id = replacement;
    (void)read_state(&f, f.guest);
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_revoke(&s, original.session_id), LG_OK); /* reclaimed tombstone */
    EQ(lg_stage_revoke(&s, 99999), LG_NOT_FOUND);
    EQ(lg_stage_rotate_guest_epoch(&s), LG_OK);
    EQ(lg_stage_admit_guest(&s, 1, f.now, 60000, &replacement), LG_REVOKED);
    EQ(lg_stage_admit_guest(&s, 2, f.now, 60000, &replacement), LG_OK);
    CHECK(replacement > f.guest.session_id);
    commit(&f, &s);
    EQ(lg_snapshot(&f.core, f.guest, 1, ++f.now, &snapshot), LG_REVOKED);
    lg_identity_t forged_epoch = {f.guest.session_id, 2};
    EQ(lg_snapshot(&f.core, forged_epoch, 1, ++f.now, &snapshot), LG_REVOKED);
    lg_identity_t bad_owner = {LG_OWNER_SESSION, 2};
    EQ(lg_snapshot(&f.core, bad_owner, 1, ++f.now, &snapshot), LG_UNAUTHORIZED);
    f.guest = (lg_identity_t){replacement, 2};
    (void)read_state(&f, f.guest);
}

static void complete_partial_and_external_changes(void) {
    fixture_t f; setup(&f);
    lg_receipt_t receipt;
    lg_intent_t out;
    lg_command_t group = command(&f, f.guest, 3, 1);
    EQ(lg_submit(&f.core, f.guest, &group, ++f.now, &receipt), LG_OK);
    EQ(lg_prepare_send(&f.core, group.id, 0, ++f.now, &out), LG_OK);
    /* A lost UDP reply may retry the exact absolute value, but rechecks first. */
    EQ(lg_prepare_send(&f.core, group.id, 0, ++f.now, &out), LG_OK);
    EQ(lg_complete_target(&f.core, group.id, 0, &out, true, ++f.now), LG_OK);
    CHECK(!f.core.lights[0].pending);
    EQ(f.core.lights[0].last_result, LG_VERIFIED);
    EQ(lg_prepare_send(&f.core, group.id, 1, ++f.now, &out), LG_OK);
    EQ(lg_complete_target(&f.core, group.id, 1, NULL, false, ++f.now), LG_OK);
    EQ(lg_command_status(&f.core, f.guest, group.id, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_PARTIAL); EQ(receipt.verified, 1); EQ(receipt.failed, 2);
    lg_command_t mismatch = command(&f, f.guest, 1, 2);
    EQ(lg_submit(&f.core, f.guest, &mismatch, ++f.now, &receipt), LG_OK);
    EQ(lg_prepare_send(&f.core, mismatch.id, 0, ++f.now, &out), LG_OK);
    out.brightness++;
    EQ(lg_complete_target(&f.core, mismatch.id, 0, &out, true, ++f.now), LG_CONFLICT);
    EQ(lg_command_status(&f.core, f.guest, mismatch.id, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_UNKNOWN);
    lg_command_t pending = command(&f, f.guest, 1, 3);
    EQ(lg_submit(&f.core, f.guest, &pending, ++f.now, &receipt), LG_OK);
    uint32_t revision = f.core.lights[0].revision;
    out.brightness = 80;
    EQ(lg_observe(&f.core, 0, revision, &out, ++f.now), LG_CONFLICT);
    EQ(lg_prepare_send(&f.core, pending.id, 0, ++f.now, &out), LG_OK);
    out.brightness = 80;
    EQ(lg_complete_target(&f.core, pending.id, 0, &out, true, ++f.now), LG_CONFLICT);
    out.brightness = 90;
    EQ(lg_observe(&f.core, 0, revision, &out, ++f.now), LG_OK);
    EQ(lg_observe(&f.core, 0, revision, &out, ++f.now), LG_CONFLICT);
    /* Owner overrides a sent command: its late readback cannot overwrite newer state. */
    pending = command(&f, f.guest, 1, 4);
    EQ(lg_submit(&f.core, f.guest, &pending, ++f.now, &receipt), LG_OK);
    EQ(lg_prepare_send(&f.core, pending.id, 0, ++f.now, &out), LG_OK);
    lg_command_t admin = command(&f, owner, 1, 1);
    admin.intent.brightness = 20;
    EQ(lg_submit(&f.core, owner, &admin, ++f.now, &receipt), LG_OK);
    EQ(lg_complete_target(&f.core, pending.id, 0, &out, true, ++f.now), LG_CONFLICT);
    EQ(f.core.lights[0].desired.brightness, 20);
}

static void deadlines_reset_and_clock(void) {
    fixture_t f; setup(&f);
    lg_receipt_t receipt;
    lg_intent_t out;
    lg_command_t a = command(&f, f.guest, 1, 1);
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_OK);
    f.now = a.expires_ms;
    EQ(lg_tick(&f.core, f.now), LG_OK);
    EQ(lg_command_status(&f.core, f.guest, a.id, f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_TIMED_OUT);
    CHECK(!f.core.lights[0].pending);
    EQ(f.core.lights[0].last_result, LG_TIMED_OUT);
    EQ(lg_prepare_send(&f.core, a.id, 0, f.now, &out), LG_EXPIRED);
    EQ(lg_submit(&f.core, f.guest, &a, f.now, &receipt), LG_DUPLICATE);
    f.now += LG_DEDUPE_GRACE_MS;
    EQ(lg_command_status(&f.core, f.guest, a.id, f.now, &receipt), LG_NOT_FOUND);
    a.expires_ms = f.now + 1000;
    EQ(lg_submit(&f.core, f.guest, &a, f.now, &receipt), LG_STALE_SEQUENCE);
    EQ(lg_tick(&f.core, f.now - 1), LG_CLOCK_ERROR);
    lg_command_t sent = command(&f, f.guest, 2, 2);
    EQ(lg_submit(&f.core, f.guest, &sent, ++f.now, &receipt), LG_OK);
    EQ(lg_prepare_send(&f.core, sent.id, 1, ++f.now, &out), LG_OK);
    f.now = sent.expires_ms;
    EQ(lg_command_status(&f.core, f.guest, sent.id, f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_UNKNOWN);
    lg_policy_stage_t stage;
    EQ(lg_policy_stage_begin(&f.core, owner, &stage, ++f.now), LG_OK);
    EQ(lg_stage_freeze(&stage, 31, f.now, 10000), LG_OK);
    commit(&f, &stage);
    lg_policy_t persisted = f.core.policy;
    EQ(lg_init(&f.core, &persisted, 43, 31), LG_OK);
    f.now = 1;
    lg_snapshot_t snapshot;
    EQ(lg_snapshot(&f.core, f.guest, 1, f.now, &snapshot), LG_EXPIRED);
    snapshot = read_state(&f, owner);
    EQ(snapshot.frozen, 31); /* prior-boot timed freezes fail closed */
    lg_command_t wrong_boot = command(&f, owner, 1, 1);
    wrong_boot.boot_id = 42;
    EQ(lg_submit(&f.core, owner, &wrong_boot, ++f.now, &receipt), LG_WRONG_BOOT);
}

static void capacity_owner_reserve_and_session_churn(void) {
    fixture_t f; setup(&f);
    lg_receipt_t receipt;
    lg_command_t commands[LG_LEDGER_COUNT];
    for (unsigned i = 0; i < LG_LEDGER_COUNT - LG_OWNER_RESERVED; ++i) {
        commands[i] = command(&f, f.guest, 1, i + 1);
        EQ(lg_submit(&f.core, f.guest, &commands[i], ++f.now, &receipt), LG_OK);
    }
    lg_command_t denied = command(&f, f.guest, 1, 100);
    EQ(lg_submit(&f.core, f.guest, &denied, ++f.now, &receipt), LG_OVERLOADED);
    for (unsigned i = 0; i < LG_OWNER_RESERVED; ++i) {
        commands[28 + i] = command(&f, owner, 2, i + 1);
        EQ(lg_submit(&f.core, owner, &commands[28 + i], ++f.now, &receipt), LG_OK);
    }
    for (unsigned i = 0; i < LG_LEDGER_COUNT; ++i)
        EQ(lg_command_status(&f.core, owner, commands[i].id, ++f.now, &receipt), LG_OK);
    denied = command(&f, owner, 4, 100);
    EQ(lg_submit(&f.core, owner, &denied, ++f.now, &receipt), LG_OVERLOADED);
    EQ(lg_next(&f.core, ++f.now, &receipt), LG_OK);
    CHECK(!memcmp(receipt.id, commands[31].id, 16));
    /* A live entry is not evicted for a new owner or guest operation. */
    EQ(lg_command_status(&f.core, owner, commands[27].id, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_QUEUED);
    setup(&f);
    lg_policy_stage_t s;
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    uint64_t id = 0;
    for (unsigned i = 1; i < LG_MAX_GUESTS; ++i)
        EQ(lg_stage_admit_guest(&s, 1, f.now, 60000, &id), LG_OK);
    EQ(lg_stage_admit_guest(&s, 1, f.now, 60000, &id), LG_OVERLOADED);
    commit(&f, &s);
    uint64_t previous = id;
    for (unsigned i = 0; i < 100; ++i) {
        EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
        EQ(lg_stage_revoke(&s, previous), LG_OK);
        EQ(lg_stage_admit_guest(&s, 1, f.now, 60000, &id), LG_OK);
        CHECK(id > previous);
        commit(&f, &s);
        previous = id;
    }
}

static uint32_t crc32_test(const uint8_t *bytes, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = crc & 1u ? (crc >> 1) ^ UINT32_C(0xedb88320) : crc >> 1;
    }
    return ~crc;
}

static void fix_crc(uint8_t record[LG_POLICY_WIRE_SIZE]) {
    uint32_t crc = crc32_test(record, LG_POLICY_WIRE_SIZE - 4);
    for (unsigned i = 0; i < 4; ++i) record[LG_POLICY_WIRE_SIZE - 4 + i] = (uint8_t)(crc >> (i * 8));
}

static void persistence_corruption_and_limits(void) {
    fixture_t f; setup(&f);
    uint8_t record[LG_POLICY_WIRE_SIZE], mutated[LG_POLICY_WIRE_SIZE];
    size_t written;
    lg_policy_t decoded;
    EQ(lg_policy_encode(&f.core.policy, record, sizeof(record), &written), LG_OK);
    for (size_t size = 0; size < sizeof(record); ++size) {
        EQ(lg_policy_decode(&decoded, record, size), LG_CORRUPT_POLICY);
        EQ(decoded.epoch, 0);
    }
    for (size_t byte = 0; byte < sizeof(record); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            memcpy(mutated, record, sizeof(record)); mutated[byte] ^= (uint8_t)(1u << bit);
            EQ(lg_policy_decode(&decoded, mutated, sizeof(mutated)), LG_CORRUPT_POLICY);
        }
    }
    memcpy(mutated, record, sizeof(record)); mutated[4] = 2; fix_crc(mutated);
    EQ(lg_policy_decode(&decoded, mutated, sizeof(mutated)), LG_CORRUPT_POLICY);
    memcpy(mutated, record, sizeof(record)); memset(mutated + 8, 0, 8); fix_crc(mutated);
    EQ(lg_policy_decode(&decoded, mutated, sizeof(mutated)), LG_CORRUPT_POLICY);
    memcpy(mutated, record, sizeof(record)); memcpy(mutated + 137, mutated + 104, 33); fix_crc(mutated);
    EQ(lg_policy_decode(&decoded, mutated, sizeof(mutated)), LG_CORRUPT_POLICY);
    memcpy(mutated, record, sizeof(record)); mutated[136] = 2; fix_crc(mutated);
    EQ(lg_policy_decode(&decoded, mutated, sizeof(mutated)), LG_CORRUPT_POLICY);
    EQ(lg_init(&f.core, &decoded, 42, 31), LG_CORRUPT_POLICY);
    EQ(lg_tick(&f.core, 0), LG_NOT_READY);
    setup(&f);
    lg_policy_stage_t s;
    f.core.policy.epoch = UINT64_MAX;
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_COUNTER_EXHAUSTED);
    f.core.policy.epoch = 1;
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    s.policy.guest_epoch = UINT64_MAX;
    EQ(lg_stage_rotate_guest_epoch(&s), LG_COUNTER_EXHAUSTED);
    s.policy.guest_epoch = 1; s.policy.next_guest_id = UINT64_MAX;
    uint64_t id;
    EQ(lg_stage_admit_guest(&s, 1, f.now, 1, &id), LG_COUNTER_EXHAUSTED);
    lg_receipt_t receipt;
    f.core.lights[0].revision = UINT32_MAX;
    lg_command_t a = command(&f, f.guest, 1, 1);
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_COUNTER_EXHAUSTED);
}

static void malformed_staging_cannot_resurrect_grants(void) {
    fixture_t f; setup(&f);
    lg_policy_stage_t s;
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_revoke(&s, f.guest.session_id), LG_OK);
    commit(&f, &s);
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    s.policy.guests[0].revoked = 0;
    EQ(lg_policy_activate_after_commit(&f.core, &s, ++f.now), LG_STALE_STAGE);
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    memset(s.policy.guests, 0, sizeof(s.policy.guests));
    s.policy.next_guest_id = 2;
    EQ(lg_policy_activate_after_commit(&f.core, &s, ++f.now), LG_STALE_STAGE);
}

static void empty_room_and_registry_edits(void) {
    lg_policy_t policy;
    lg_core_t empty;
    lg_policy_factory(&policy, 42);
    EQ(lg_init(&empty, &policy, 42, 0), LG_OK);
    lg_snapshot_t snapshot;
    EQ(lg_snapshot(&empty, owner, 0, 1, &snapshot), LG_OK);
    EQ(snapshot.enabled, 0);
    EQ(lg_snapshot(&empty, owner, 1, 2, &snapshot), LG_BAD_ARGUMENT);
    EQ(lg_validate_enabled_change(&empty, owner, 1, 3), LG_OK);
    EQ(empty.enabled, 0); /* preflight must not adopt before durable write */
    EQ(lg_set_enabled_after_commit(&empty, owner, 1, 4), LG_OK);
    EQ(empty.lights[0].revision, 2);
    fixture_t f; setup(&f);
    lg_command_t a = command(&f, f.guest, 1, 1);
    lg_receipt_t receipt;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_OK);
    uint32_t old_revision = f.core.lights[0].revision;
    EQ(lg_validate_enabled_change(&f.core, f.guest, 30, ++f.now), LG_UNAUTHORIZED);
    EQ(lg_validate_enabled_change(&f.core, owner, 30, ++f.now), LG_OK);
    EQ(lg_set_enabled_after_commit(&f.core, owner, 30, ++f.now), LG_OK);
    EQ(lg_set_enabled_after_commit(&f.core, owner, 31, ++f.now), LG_OK);
    EQ(f.core.lights[0].revision, old_revision + 2);
    CHECK(!f.core.lights[0].observed_valid);
    EQ(lg_command_status(&f.core, f.guest, a.id, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_SUPERSEDED);
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_DUPLICATE);
    snapshot = read_state(&f, f.guest);
    EQ(snapshot.last_sequence, 1);
    EQ(lg_observe(&f.core, 0, old_revision, &a.intent, ++f.now), LG_CONFLICT);
    f.core.lights[3].revision = UINT32_MAX;
    EQ(lg_validate_enabled_change(&f.core, owner, 0, ++f.now), LG_COUNTER_EXHAUSTED);
    EQ(f.core.enabled, 31);
    EQ(lg_set_enabled_after_commit(&f.core, owner, 0, ++f.now), LG_COUNTER_EXHAUSTED);
    EQ(f.core.enabled, 31);
}

static void canceled_group_does_not_leave_pending_ui(void) {
    fixture_t f; setup(&f);
    lg_receipt_t receipt;
    lg_command_t group = command(&f, f.guest, 3, 1);
    EQ(lg_submit(&f.core, f.guest, &group, ++f.now, &receipt), LG_OK);
    CHECK(f.core.lights[0].pending && f.core.lights[1].pending);
    lg_command_t admin = command(&f, owner, 1, 1);
    EQ(lg_submit(&f.core, owner, &admin, ++f.now, &receipt), LG_OK);
    CHECK(f.core.lights[0].pending);
    CHECK(!f.core.lights[1].pending);
    EQ(f.core.lights[1].last_result, LG_SUPERSEDED);
}

static void checks_do_not_claim_physical_sends(void) {
    fixture_t f; setup(&f);
    lg_receipt_t receipt;
    lg_command_t request = command(&f, f.guest, 1, 1);
    EQ(lg_submit(&f.core, f.guest, &request, ++f.now, &receipt), LG_OK);
    lg_intent_t out;
    EQ(lg_check_send(&f.core, request.id, 0, ++f.now, &out), LG_OK);
    EQ(lg_check_send(&f.core, request.id, 0, ++f.now, &out), LG_OK);
    EQ(lg_command_status(&f.core, f.guest, request.id, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_QUEUED); EQ(receipt.sent, 0);
    /* Encoding/key/socket preflight failed; no packet left the gateway. */
    EQ(lg_complete_target(&f.core, request.id, 0, NULL, false, ++f.now), LG_OK);
    EQ(lg_command_status(&f.core, f.guest, request.id, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_REJECTED); EQ(receipt.sent, 0);
    EQ(f.core.lights[0].last_result, LG_REJECTED); CHECK(!f.core.lights[0].pending);
    request = command(&f, f.guest, 1, 2);
    EQ(lg_submit(&f.core, f.guest, &request, ++f.now, &receipt), LG_OK);
    EQ(lg_check_send(&f.core, request.id, 0, ++f.now, &out), LG_OK);
    EQ(lg_complete_target(&f.core, request.id, 0, &out, true, ++f.now), LG_BAD_ARGUMENT);
    /* The actual successful UDP send is now reported, still not delivery. */
    EQ(lg_prepare_send(&f.core, request.id, 0, ++f.now, &out), LG_OK);
    EQ(lg_complete_target(&f.core, request.id, 0, NULL, false, ++f.now), LG_OK);
    EQ(lg_command_status(&f.core, f.guest, request.id, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_UNKNOWN); EQ(receipt.sent, 1);
}

static void boot_renewal_preserves_slot_and_rejects_resurrection(void) {
    fixture_t f; setup(&f);
    lg_policy_stage_t s;
    uint64_t old_id = 0, new_id = 0;
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_admit_guest(&s, 1, f.now, 60000, &old_id), LG_OK);
    commit(&f, &s);
    EQ(f.core.policy.guests[1].id, old_id);
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_revoke(&s, f.guest.session_id), LG_OK);
    commit(&f, &s);
    lg_policy_t durable = f.core.policy;
    EQ(lg_init(&f.core, &durable, 43, 31), LG_OK);
    f.now = 1;
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_renew_guest(&s, f.guest.session_id, 1, f.now, 60000, &new_id), LG_REVOKED);
    EQ(lg_stage_renew_guest(&s, old_id, 2, f.now, 60000, &new_id), LG_REVOKED);
    EQ(lg_stage_renew_guest(&s, old_id, 1, f.now, LG_MAX_GUEST_TTL_MS + 1, &new_id), LG_BAD_ARGUMENT);
    EQ(lg_stage_renew_guest(&s, old_id, 1, f.now, 60000, &new_id), LG_OK);
    CHECK(new_id > old_id);
    EQ(s.policy.guests[1].id, new_id); /* do not use the earlier revoked slot */
    EQ(s.policy.guests[0].id, f.guest.session_id);
    CHECK(s.policy.guests[0].revoked);
    EQ(s.policy.guests[1].boot_id, 43);
    EQ(lg_stage_renew_guest(&s, old_id, 1, f.now, 60000, &new_id), LG_REVOKED);
    uint64_t renewed = new_id;
    commit(&f, &s);
    lg_snapshot_t snapshot;
    EQ(lg_snapshot(&f.core, (lg_identity_t){old_id,1}, 0, ++f.now, &snapshot), LG_REVOKED);
    EQ(lg_snapshot(&f.core, (lg_identity_t){renewed,1}, 0, ++f.now, &snapshot), LG_OK);
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_renew_guest(&s, renewed, 1, f.now, 60000, &new_id), LG_BAD_ARGUMENT);
    /* Even expired same-boot sessions require explicit admission, not renewal. */
    f.now += 60001;
    EQ(lg_stage_renew_guest(&s, renewed, 1, f.now, 60000, &new_id), LG_BAD_ARGUMENT);
    durable = f.core.policy;
    EQ(lg_init(&f.core, &durable, 44, 31), LG_OK); f.now = 1;
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    s.policy.next_guest_id = UINT64_MAX;
    EQ(lg_stage_renew_guest(&s, renewed, 1, f.now, 60000, &new_id), LG_COUNTER_EXHAUSTED);
    EQ(lg_stage_rotate_guest_epoch(&s), LG_OK);
    EQ(lg_stage_renew_guest(&s, renewed, 2, f.now, 60000, &new_id), LG_REVOKED);
}

static void malformed_commands_and_inflight_revocation(void) {
    fixture_t f; setup(&f);
    lg_receipt_t receipt;
    lg_command_t good = command(&f, f.guest, 1, 1);
    lg_command_t a = good;
    a.intent.fields |= LG_RGB | LG_TEMPERATURE; a.intent.kelvin = 3000;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_BAD_ARGUMENT);
    a = good; a.intent.brightness = 101;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_BAD_ARGUMENT);
    a = good; a.intent.red = 1;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_BAD_ARGUMENT);
    a = good; a.targets = 128;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_BAD_ARGUMENT);
    a = good; a.expires_ms = f.now;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_EXPIRED);
    a = good; a.expires_ms = f.now + 50000;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_BAD_ARGUMENT);
    a = good; a.expected_revision[7] = 1;
    EQ(lg_submit(&f.core, f.guest, &a, ++f.now, &receipt), LG_BAD_ARGUMENT);
    lg_intent_t out;
    EQ(lg_submit(&f.core, f.guest, &good, ++f.now, &receipt), LG_OK);
    EQ(lg_prepare_send(&f.core, good.id, 0, ++f.now, &out), LG_OK);
    lg_policy_stage_t s;
    EQ(lg_policy_stage_begin(&f.core, owner, &s, ++f.now), LG_OK);
    EQ(lg_stage_revoke(&s, f.guest.session_id), LG_OK);
    commit(&f, &s);
    EQ(lg_prepare_send(&f.core, good.id, 0, ++f.now, &out), LG_POLICY_CHANGED);
    EQ(lg_command_status(&f.core, owner, good.id, ++f.now, &receipt), LG_OK);
    EQ(receipt.result, LG_UNKNOWN);
    EQ(lg_command_status(&f.core, f.guest, good.id, ++f.now, &receipt), LG_REVOKED);
}

int main(void) {
    duplicate_and_ordering();
    conflicts_require_reads();
    policy_staging_and_freezes();
    owner_priority_and_holds();
    revoke_rotation_and_no_id_reuse();
    complete_partial_and_external_changes();
    deadlines_reset_and_clock();
    capacity_owner_reserve_and_session_churn();
    persistence_corruption_and_limits();
    malformed_commands_and_inflight_revocation();
    malformed_staging_cannot_resurrect_grants();
    empty_room_and_registry_edits();
    canceled_group_does_not_leave_pending_ui();
    checks_do_not_claim_physical_sends();
    boot_renewal_preserves_slot_and_rejects_resurrection();
    printf("PASS: %u assertions; core=%zu bytes, policy=%zu, stage=%zu, wire=%u, ledger entry=%zu\n",
           checks, sizeof(lg_core_t), sizeof(lg_policy_t), sizeof(lg_policy_stage_t),
           LG_POLICY_WIRE_SIZE, sizeof(lg_ledger_entry_t));
    return 0;
}
