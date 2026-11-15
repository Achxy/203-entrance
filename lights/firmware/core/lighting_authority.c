#include "lighting_authority.h"

#include <limits.h>
#include <string.h>

static bool nonzero(const uint8_t *p, size_t size) {
    uint8_t bits = 0;
    for (size_t i = 0; i < size; ++i) bits |= p[i];
    return bits != 0;
}

static bool intent_valid(const lg_intent_t *s) {
    if (!s || !s->fields || (s->fields & ~63u)) return false;
    unsigned modes = ((s->fields & LG_RGB) ? 1u : 0u) +
                     ((s->fields & LG_TEMPERATURE) ? 1u : 0u) +
                     ((s->fields & LG_SCENE) ? 1u : 0u);
    return modes <= 1 &&
           ((s->fields & LG_POWER) ? s->power <= 1 : s->power == 0) &&
           ((s->fields & LG_BRIGHTNESS) ? (s->brightness >= 1 && s->brightness <= 100) : s->brightness == 0) &&
           ((s->fields & LG_RGB) || !(s->red || s->green || s->blue)) &&
           ((s->fields & LG_TEMPERATURE) ? (s->kelvin >= 1000 && s->kelvin <= 10000) : s->kelvin == 0) &&
           ((s->fields & LG_SCENE) ? s->scene > 0 : s->scene == 0) &&
           ((s->fields & LG_SPEED) ? (s->speed >= 1 && s->speed <= 200) : s->speed == 0);
}

static bool intent_equal(const lg_intent_t *a, const lg_intent_t *b) {
    return a->fields == b->fields && a->kelvin == b->kelvin &&
           a->scene == b->scene && a->power == b->power &&
           a->brightness == b->brightness && a->red == b->red &&
           a->green == b->green && a->blue == b->blue && a->speed == b->speed;
}

static void merge_intent(lg_intent_t *dst, const lg_intent_t *src) {
    if (src->fields & (LG_RGB | LG_TEMPERATURE | LG_SCENE)) {
        dst->fields &= (uint16_t)~(LG_RGB | LG_TEMPERATURE | LG_SCENE);
        dst->red = dst->green = dst->blue = 0;
        dst->kelvin = dst->scene = 0;
    }
    if (src->fields & LG_POWER) dst->power = src->power;
    if (src->fields & LG_BRIGHTNESS) dst->brightness = src->brightness;
    if (src->fields & LG_RGB) {
        dst->red = src->red; dst->green = src->green; dst->blue = src->blue;
    }
    if (src->fields & LG_TEMPERATURE) dst->kelvin = src->kelvin;
    if (src->fields & LG_SCENE) dst->scene = src->scene;
    if (src->fields & LG_SPEED) dst->speed = src->speed;
    dst->fields |= src->fields;
}

static bool matches(const lg_intent_t *desired, const lg_intent_t *observed) {
    if ((observed->fields & desired->fields) != desired->fields) return false;
    lg_intent_t projected = {0};
    projected.fields = desired->fields;
    if (desired->fields & LG_POWER) projected.power = observed->power;
    if (desired->fields & LG_BRIGHTNESS) projected.brightness = observed->brightness;
    if (desired->fields & LG_RGB) {
        projected.red = observed->red; projected.green = observed->green; projected.blue = observed->blue;
    }
    if (desired->fields & LG_TEMPERATURE) projected.kelvin = observed->kelvin;
    if (desired->fields & LG_SCENE) projected.scene = observed->scene;
    if (desired->fields & LG_SPEED) projected.speed = observed->speed;
    return intent_equal(desired, &projected);
}

static bool policy_valid(const lg_policy_t *p) {
    if (!p || !p->epoch || !p->guest_epoch || p->next_guest_id < 2 || !p->clock_boot_id) return false;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        const lg_guest_t *g = &p->guests[i];
        if (!g->id) {
            if (g->epoch || g->boot_id || g->expires_ms || g->revoked) return false;
            continue;
        }
        if (g->id < 2 || g->id >= p->next_guest_id || !g->epoch ||
            g->epoch > p->guest_epoch || !g->boot_id || !g->expires_ms || g->revoked > 1) return false;
        for (size_t j = 0; j < i; ++j) if (p->guests[j].id == g->id) return false;
    }
    return true;
}

static bool live(const lg_ledger_entry_t *e) {
    return e->result == LG_QUEUED || e->result == LG_SENT;
}

static void terminal_targets(lg_core_t *c, const lg_ledger_entry_t *e, lg_result_t result) {
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
        uint8_t bit = (uint8_t)(1u << i);
        if ((e->command.targets & bit) && !((e->verified | e->failed) & bit) &&
            e->command.expected_revision[i] != UINT32_MAX &&
            c->lights[i].revision == e->command.expected_revision[i] + 1) {
            c->lights[i].pending = false;
            c->lights[i].last_result = result;
        }
    }
}

static void cancel(lg_core_t *c, lg_ledger_entry_t *e, lg_error_t reason) {
    e->error = reason;
    e->result = e->sent ? LG_UNKNOWN : LG_SUPERSEDED;
    terminal_targets(c, e, e->result);
}

lg_error_t lg_tick(lg_core_t *c, uint64_t now) {
    if (!c || !c->ready) return LG_NOT_READY;
    if (now < c->last_now_ms) return LG_CLOCK_ERROR;
    c->last_now_ms = now;
    for (size_t i = 0; i < LG_LEDGER_COUNT; ++i) {
        lg_ledger_entry_t *e = &c->ledger[i];
        if (live(e) && now >= e->command.expires_ms) {
            e->error = LG_EXPIRED;
            e->result = e->sent ? LG_UNKNOWN : LG_TIMED_OUT;
            terminal_targets(c, e, e->result);
        }
        if (e->result != LG_EMPTY && !live(e) && now >= e->retain_until_ms)
            memset(e, 0, sizeof(*e));
    }
    return LG_OK;
}

static lg_error_t authorize(lg_core_t *c, lg_identity_t id, uint64_t now,
                            lg_sequence_t **sequence) {
    if (id.session_id == LG_OWNER_SESSION) {
        if (id.guest_epoch != 0) return LG_UNAUTHORIZED;
        if (sequence) *sequence = &c->owner;
        return LG_OK;
    }
    if (id.session_id < 2 || id.guest_epoch != c->policy.guest_epoch) return LG_REVOKED;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        const lg_guest_t *g = &c->policy.guests[i];
        if (g->id != id.session_id) continue;
        if (g->revoked || g->epoch != id.guest_epoch) return LG_REVOKED;
        if (g->boot_id != c->boot_id || now >= g->expires_ms) return LG_EXPIRED;
        if (sequence) *sequence = &c->guests[i];
        return LG_OK;
    }
    return LG_REVOKED;
}

static uint8_t freeze_mask(const lg_core_t *c, uint64_t now) {
    uint8_t mask = 0;
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
        uint64_t deadline = c->policy.freeze_until_ms[i];
        if (deadline && (c->policy.clock_boot_id != c->boot_id || deadline == UINT64_MAX || now < deadline))
            mask |= (uint8_t)(1u << i);
    }
    return mask;
}

static uint8_t hold_mask(const lg_core_t *c, uint64_t now) {
    uint8_t mask = 0;
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i)
        if (now < c->owner_hold_until_ms[i]) mask |= (uint8_t)(1u << i);
    return mask;
}

void lg_policy_factory(lg_policy_t *p, uint64_t boot_id) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->epoch = p->guest_epoch = 1;
    p->next_guest_id = 2;
    p->clock_boot_id = boot_id;
}

lg_error_t lg_init(lg_core_t *c, const lg_policy_t *p, uint64_t boot_id, uint8_t enabled) {
    if (!c) return LG_BAD_ARGUMENT;
    memset(c, 0, sizeof(*c));
    if (!policy_valid(p)) return LG_CORRUPT_POLICY;
    if (!boot_id) return LG_BAD_ARGUMENT;
    c->policy = *p;
    c->boot_id = boot_id;
    c->enabled = enabled;
    c->owner.session_id = LG_OWNER_SESSION;
    c->owner.needs_read = enabled;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        c->guests[i].session_id = p->guests[i].id;
        c->guests[i].needs_read = enabled;
    }
    for (size_t i = 0; i < LG_MAX_LIGHTS; ++i) c->lights[i].revision = 1;
    c->ready = true;
    return LG_OK;
}

lg_error_t lg_policy_stage_begin(lg_core_t *c, lg_identity_t owner,
                                lg_policy_stage_t *s, uint64_t now) {
    if (!s) return LG_BAD_ARGUMENT;
    memset(s, 0, sizeof(*s));
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    if (owner.session_id != LG_OWNER_SESSION || authorize(c, owner, now, NULL) != LG_OK)
        return LG_UNAUTHORIZED;
    if (c->policy.epoch == UINT64_MAX) return LG_COUNTER_EXHAUSTED;
    s->policy = c->policy;
    s->base_epoch = c->policy.epoch;
    s->boot_id = c->boot_id;
    s->now_ms = now;
    s->policy.epoch++;
    if (s->policy.clock_boot_id != c->boot_id) {
        for (size_t i = 0; i < LG_MAX_LIGHTS; ++i)
            if (s->policy.freeze_until_ms[i]) s->policy.freeze_until_ms[i] = UINT64_MAX;
        s->policy.clock_boot_id = c->boot_id;
    }
    s->valid = true;
    return LG_OK;
}

lg_error_t lg_stage_admit_guest(lg_policy_stage_t *s, uint64_t proven_epoch,
                               uint64_t now, uint64_t ttl, uint64_t *session_id) {
    if (!s || !s->valid || !session_id) return LG_BAD_ARGUMENT;
    if (now < s->now_ms) return LG_CLOCK_ERROR;
    if (proven_epoch != s->policy.guest_epoch) return LG_REVOKED;
    if (!ttl || ttl > LG_MAX_GUEST_TTL_MS || now > UINT64_MAX - ttl) return LG_BAD_ARGUMENT;
    if (s->policy.next_guest_id == UINT64_MAX) return LG_COUNTER_EXHAUSTED;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        lg_guest_t *g = &s->policy.guests[i];
        if (g->id && !g->revoked && g->epoch == s->policy.guest_epoch &&
            g->boot_id == s->boot_id && now < g->expires_ms) continue;
        *g = (lg_guest_t){.id = s->policy.next_guest_id++, .epoch = proven_epoch,
                          .boot_id = s->boot_id, .expires_ms = now + ttl};
        *session_id = g->id;
        s->now_ms = now;
        return LG_OK;
    }
    return LG_OVERLOADED;
}

lg_error_t lg_stage_revoke(lg_policy_stage_t *s, uint64_t session_id) {
    if (!s || !s->valid || session_id < 2) return LG_BAD_ARGUMENT;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        if (s->policy.guests[i].id != session_id) continue;
        s->policy.guests[i].revoked = 1;
        return LG_OK;
    }
    /* IDs below the durable high-water mark can never be granted again. */
    return session_id < s->policy.next_guest_id ? LG_OK : LG_NOT_FOUND;
}

lg_error_t lg_stage_renew_guest(lg_policy_stage_t *s, uint64_t old_id,
                               uint64_t proven_epoch, uint64_t now, uint64_t ttl,
                               uint64_t *new_id) {
    if (!s || !s->valid || !new_id || old_id < 2) return LG_BAD_ARGUMENT;
    if (now < s->now_ms) return LG_CLOCK_ERROR;
    if (proven_epoch != s->policy.guest_epoch) return LG_REVOKED;
    if (!ttl || ttl > LG_MAX_GUEST_TTL_MS || now > UINT64_MAX - ttl) return LG_BAD_ARGUMENT;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        lg_guest_t *g = &s->policy.guests[i];
        if (g->id != old_id) continue;
        if (g->revoked || g->epoch != proven_epoch) return LG_REVOKED;
        if (g->boot_id == s->boot_id) return LG_BAD_ARGUMENT;
        if (s->policy.next_guest_id == UINT64_MAX) return LG_COUNTER_EXHAUSTED;
        g->id = s->policy.next_guest_id++;
        g->boot_id = s->boot_id;
        g->expires_ms = now + ttl;
        *new_id = g->id;
        s->now_ms = now;
        return LG_OK;
    }
    return LG_REVOKED;
}

lg_error_t lg_stage_rotate_guest_epoch(lg_policy_stage_t *s) {
    if (!s || !s->valid) return LG_BAD_ARGUMENT;
    if (s->policy.guest_epoch == UINT64_MAX) return LG_COUNTER_EXHAUSTED;
    s->policy.guest_epoch++;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i)
        if (s->policy.guests[i].id) s->policy.guests[i].revoked = 1;
    return LG_OK;
}

lg_error_t lg_stage_freeze(lg_policy_stage_t *s, uint8_t targets,
                          uint64_t now, uint64_t duration) {
    if (!s || !s->valid || !targets) return LG_BAD_ARGUMENT;
    if (now < s->now_ms) return LG_CLOCK_ERROR;
    if (duration != UINT64_MAX && (duration > LG_MAX_GUEST_TTL_MS || now > UINT64_MAX - duration))
        return LG_BAD_ARGUMENT;
    uint64_t until = duration == UINT64_MAX ? UINT64_MAX : duration ? now + duration : 0;
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i)
        if (targets & (1u << i)) s->policy.freeze_until_ms[i] = until;
    s->now_ms = now;
    return LG_OK;
}

lg_error_t lg_policy_activate_after_commit(lg_core_t *c, const lg_policy_stage_t *s, uint64_t now) {
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    if (!s || !s->valid || !policy_valid(&s->policy)) return LG_BAD_ARGUMENT;
    if (s->boot_id != c->boot_id || s->base_epoch != c->policy.epoch ||
        s->base_epoch == UINT64_MAX || s->policy.epoch != s->base_epoch + 1 ||
        s->policy.clock_boot_id != c->boot_id || s->now_ms > now) return LG_STALE_STAGE;
    if (s->policy.guest_epoch < c->policy.guest_epoch ||
        s->policy.next_guest_id < c->policy.next_guest_id) return LG_STALE_STAGE;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        const lg_guest_t *candidate = &s->policy.guests[i];
        if (!candidate->id || candidate->id >= c->policy.next_guest_id) continue;
        const lg_guest_t *prior = NULL;
        for (size_t j = 0; j < LG_MAX_GUESTS; ++j)
            if (c->policy.guests[j].id == candidate->id) prior = &c->policy.guests[j];
        if (!prior || prior->epoch != candidate->epoch || prior->boot_id != candidate->boot_id ||
            prior->expires_ms != candidate->expires_ms || (prior->revoked && !candidate->revoked))
            return LG_STALE_STAGE;
    }
    lg_sequence_t previous[LG_MAX_GUESTS];
    memcpy(previous, c->guests, sizeof(previous));
    c->policy = s->policy;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        c->guests[i] = (lg_sequence_t){.session_id = c->policy.guests[i].id, .needs_read = c->enabled};
        for (size_t j = 0; j < LG_MAX_GUESTS; ++j)
            if (previous[j].session_id == c->guests[i].session_id)
                c->guests[i].sequence = previous[j].sequence;
    }
    c->owner.needs_read = c->enabled;
    for (size_t i = 0; i < LG_LEDGER_COUNT; ++i)
        if (live(&c->ledger[i])) cancel(c, &c->ledger[i], LG_POLICY_CHANGED);
    return LG_OK;
}

lg_error_t lg_clear_owner_holds(lg_core_t *c, lg_identity_t owner, uint8_t targets, uint64_t now) {
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    if (!targets || (targets & ~c->enabled)) return LG_BAD_ARGUMENT;
    if (owner.session_id != LG_OWNER_SESSION || authorize(c, owner, now, NULL) != LG_OK)
        return LG_UNAUTHORIZED;
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i)
        if (targets & (1u << i)) c->owner_hold_until_ms[i] = 0;
    return LG_OK;
}

lg_error_t lg_validate_enabled_change(lg_core_t *c, lg_identity_t owner, uint8_t enabled, uint64_t now) {
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    if (owner.session_id != LG_OWNER_SESSION || authorize(c, owner, now, NULL) != LG_OK)
        return LG_UNAUTHORIZED;
    uint8_t changed = (uint8_t)(c->enabled ^ enabled);
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i)
        if ((changed & (1u << i)) && c->lights[i].revision == UINT32_MAX) return LG_COUNTER_EXHAUSTED;
    return LG_OK;
}

lg_error_t lg_set_enabled_after_commit(lg_core_t *c, lg_identity_t owner, uint8_t enabled, uint64_t now) {
    lg_error_t error = lg_validate_enabled_change(c, owner, enabled, now);
    if (error != LG_OK) return error;
    uint8_t changed = (uint8_t)(c->enabled ^ enabled);
    if (!changed) return LG_OK;
    for (size_t i = 0; i < LG_LEDGER_COUNT; ++i)
        if (live(&c->ledger[i]) && (c->ledger[i].command.targets & changed))
            cancel(c, &c->ledger[i], LG_CONFLICT);
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
        if (!(changed & (1u << i))) continue;
        uint32_t revision = c->lights[i].revision + 1;
        memset(&c->lights[i], 0, sizeof(c->lights[i]));
        c->lights[i].revision = revision;
        c->owner_hold_until_ms[i] = 0;
    }
    c->enabled = enabled;
    c->owner.needs_read = enabled;
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) c->guests[i].needs_read = enabled;
    return LG_OK;
}

lg_error_t lg_snapshot(lg_core_t *c, lg_identity_t identity, uint8_t targets,
                       uint64_t now, lg_snapshot_t *snapshot) {
    if (!snapshot) return LG_BAD_ARGUMENT;
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    if (targets & ~c->enabled) return LG_BAD_ARGUMENT;
    lg_sequence_t *sequence = NULL;
    error = authorize(c, identity, now, &sequence);
    if (error != LG_OK) return error;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->boot_id = c->boot_id;
    snapshot->policy_epoch = c->policy.epoch;
    snapshot->guest_epoch = c->policy.guest_epoch;
    snapshot->last_sequence = sequence->sequence;
    snapshot->enabled = c->enabled;
    snapshot->frozen = freeze_mask(c, now);
    snapshot->owner_held = hold_mask(c, now);
    memcpy(snapshot->lights, c->lights, sizeof(snapshot->lights));
    sequence->needs_read &= (uint8_t)~targets;
    return LG_OK;
}

static lg_ledger_entry_t *find(lg_core_t *c, const uint8_t id[16]) {
    for (size_t i = 0; i < LG_LEDGER_COUNT; ++i)
        if (c->ledger[i].result != LG_EMPTY && !memcmp(c->ledger[i].command.id, id, 16))
            return &c->ledger[i];
    return NULL;
}

static void receipt_from(const lg_ledger_entry_t *e, lg_receipt_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->id, e->command.id, 16);
    out->result = e->result; out->error = e->error;
    out->sent = e->sent; out->verified = e->verified; out->failed = e->failed;
}

static bool command_equal(const lg_command_t *a, const lg_command_t *b) {
    return !memcmp(a->id, b->id, 16) && !memcmp(a->digest, b->digest, 32) &&
           a->boot_id == b->boot_id && a->policy_epoch == b->policy_epoch &&
           a->sequence == b->sequence && a->expires_ms == b->expires_ms &&
           !memcmp(a->expected_revision, b->expected_revision, sizeof(a->expected_revision)) &&
           a->owner_hold_ms == b->owner_hold_ms && a->targets == b->targets &&
           intent_equal(&a->intent, &b->intent);
}

static lg_error_t reject(lg_ledger_entry_t *e, lg_error_t error, lg_receipt_t *out) {
    e->result = LG_REJECTED; e->error = error; receipt_from(e, out); return error;
}

static void invalidate_overlap(lg_core_t *c, uint8_t targets, const lg_ledger_entry_t *except) {
    for (size_t i = 0; i < LG_LEDGER_COUNT; ++i) {
        lg_ledger_entry_t *e = &c->ledger[i];
        if (e != except && live(e) && (e->command.targets & targets)) cancel(c, e, LG_CONFLICT);
    }
}

lg_error_t lg_submit(lg_core_t *c, lg_identity_t identity,
                     const lg_command_t *command, uint64_t now, lg_receipt_t *receipt) {
    if (!command || !receipt) return LG_BAD_ARGUMENT;
    memset(receipt, 0, sizeof(*receipt));
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    lg_sequence_t *sequence = NULL;
    error = authorize(c, identity, now, &sequence);
    if (error != LG_OK) return error;
    lg_ledger_entry_t *old = find(c, command->id);
    if (old) {
        if (old->session_id != identity.session_id || old->guest_epoch != identity.guest_epoch ||
            !command_equal(&old->command, command)) return LG_ID_REUSE;
        receipt_from(old, receipt);
        return LG_DUPLICATE;
    }
    if (!nonzero(command->id, 16) || !nonzero(command->digest, 32) || !command->sequence ||
        !command->targets || (command->targets & ~c->enabled) || !intent_valid(&command->intent) ||
        command->owner_hold_ms > LG_MAX_OWNER_HOLD_MS ||
        (identity.session_id != LG_OWNER_SESSION && command->owner_hold_ms)) return LG_BAD_ARGUMENT;
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i)
        if (!(command->targets & (1u << i)) && command->expected_revision[i]) return LG_BAD_ARGUMENT;
    if (command->expires_ms <= now) return LG_EXPIRED;
    if (now > UINT64_MAX - LG_MAX_COMMAND_TTL_MS - LG_DEDUPE_GRACE_MS ||
        command->expires_ms - now > LG_MAX_COMMAND_TTL_MS ||
        now > UINT64_MAX - command->owner_hold_ms) return LG_BAD_ARGUMENT;
    lg_ledger_entry_t *entry = NULL;
    size_t start = identity.session_id == LG_OWNER_SESSION ? 0 : LG_OWNER_RESERVED;
    for (size_t i = start; i < LG_LEDGER_COUNT; ++i)
        if (c->ledger[i].result == LG_EMPTY) { entry = &c->ledger[i]; break; }
    if (!entry) return LG_OVERLOADED;
    memset(entry, 0, sizeof(*entry));
    entry->command = *command; entry->session_id = identity.session_id;
    entry->guest_epoch = identity.guest_epoch;
    entry->retain_until_ms = command->expires_ms + LG_DEDUPE_GRACE_MS;
    if (command->boot_id != c->boot_id) return reject(entry, LG_WRONG_BOOT, receipt);
    if (command->policy_epoch != c->policy.epoch) {
        sequence->needs_read |= command->targets;
        return reject(entry, LG_POLICY_CHANGED, receipt);
    }
    if (command->sequence <= sequence->sequence) return reject(entry, LG_STALE_SEQUENCE, receipt);
    if (sequence->needs_read & command->targets) return reject(entry, LG_FRESH_READ_REQUIRED, receipt);
    if (identity.session_id != LG_OWNER_SESSION) {
        if (freeze_mask(c, now) & command->targets) return reject(entry, LG_FROZEN, receipt);
        if (hold_mask(c, now) & command->targets) return reject(entry, LG_OWNER_HOLD, receipt);
    }
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
        if (!(command->targets & (1u << i))) continue;
        if (command->expected_revision[i] != c->lights[i].revision) {
            sequence->needs_read |= command->targets;
            return reject(entry, LG_CONFLICT, receipt);
        }
        if (c->lights[i].revision == UINT32_MAX) return reject(entry, LG_COUNTER_EXHAUSTED, receipt);
    }
    entry->result = LG_QUEUED;
    sequence->sequence = command->sequence;
    invalidate_overlap(c, command->targets, entry);
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i) {
        if (!(command->targets & (1u << i))) continue;
        c->lights[i].revision++;
        merge_intent(&c->lights[i].desired, &command->intent);
        c->lights[i].pending = true;
        c->lights[i].last_result = LG_QUEUED;
        if (identity.session_id == LG_OWNER_SESSION && command->owner_hold_ms)
            c->owner_hold_until_ms[i] = now + command->owner_hold_ms;
    }
    receipt_from(entry, receipt);
    return LG_OK;
}

lg_error_t lg_command_status(lg_core_t *c, lg_identity_t identity, const uint8_t id[16],
                             uint64_t now, lg_receipt_t *receipt) {
    if (!id || !receipt) return LG_BAD_ARGUMENT;
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    error = authorize(c, identity, now, NULL);
    if (error != LG_OK) return error;
    lg_ledger_entry_t *e = find(c, id);
    if (!e || (identity.session_id != LG_OWNER_SESSION && e->session_id != identity.session_id))
        return LG_NOT_FOUND;
    receipt_from(e, receipt);
    return LG_OK;
}

static lg_error_t recheck(lg_core_t *c, lg_ledger_entry_t *e, uint64_t now) {
    lg_identity_t identity = {e->session_id, e->guest_epoch};
    lg_error_t error = authorize(c, identity, now, NULL);
    if (error != LG_OK) return error;
    if (e->command.policy_epoch != c->policy.epoch) return LG_POLICY_CHANGED;
    if (now >= e->command.expires_ms) return LG_EXPIRED;
    if (e->session_id != LG_OWNER_SESSION) {
        if (freeze_mask(c, now) & e->command.targets) return LG_FROZEN;
        if (hold_mask(c, now) & e->command.targets) return LG_OWNER_HOLD;
    }
    for (unsigned i = 0; i < LG_MAX_LIGHTS; ++i)
        if ((e->command.targets & (1u << i)) &&
            (e->command.expected_revision[i] == UINT32_MAX ||
             c->lights[i].revision != e->command.expected_revision[i] + 1)) return LG_CONFLICT;
    return LG_OK;
}

lg_error_t lg_next(lg_core_t *c, uint64_t now, lg_receipt_t *receipt) {
    if (!receipt) return LG_BAD_ARGUMENT;
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < LG_LEDGER_COUNT; ++i) {
            lg_ledger_entry_t *e = &c->ledger[i];
            if (!live(e) || (e->session_id == LG_OWNER_SESSION) != (pass == 0)) continue;
            error = recheck(c, e, now);
            if (error != LG_OK) { cancel(c, e, error); continue; }
            receipt_from(e, receipt);
            return LG_OK;
        }
    }
    memset(receipt, 0, sizeof(*receipt));
    return LG_IDLE;
}

lg_error_t lg_check_send(lg_core_t *c, const uint8_t id[16], uint8_t index,
                        uint64_t now, lg_intent_t *intent) {
    if (!id || !intent || index >= LG_MAX_LIGHTS) return LG_BAD_ARGUMENT;
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    lg_ledger_entry_t *e = find(c, id);
    if (!e) return LG_NOT_FOUND;
    uint8_t bit = (uint8_t)(1u << index);
    if (!live(e)) return e->error == LG_OK ? LG_CONFLICT : e->error;
    if (!(e->command.targets & bit) || ((e->verified | e->failed) & bit)) return LG_BAD_ARGUMENT;
    error = recheck(c, e, now);
    if (error != LG_OK) { cancel(c, e, error); return error; }
    *intent = e->command.intent;
    return LG_OK;
}

lg_error_t lg_prepare_send(lg_core_t *c, const uint8_t id[16], uint8_t index,
                           uint64_t now, lg_intent_t *intent) {
    lg_error_t error = lg_check_send(c, id, index, now, intent);
    if (error != LG_OK) return error;
    lg_ledger_entry_t *e = find(c, id);
    uint8_t bit = (uint8_t)(1u << index);
    e->result = LG_SENT; e->sent |= bit;
    c->lights[index].last_result = LG_SENT;
    return LG_OK;
}

lg_error_t lg_complete_target(lg_core_t *c, const uint8_t id[16], uint8_t index,
                              const lg_intent_t *observed, bool verified, uint64_t now) {
    if (!id || index >= LG_MAX_LIGHTS || (observed && !intent_valid(observed)) ||
        (verified && !observed)) return LG_BAD_ARGUMENT;
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    lg_ledger_entry_t *e = find(c, id);
    if (!e) return LG_NOT_FOUND;
    uint8_t bit = (uint8_t)(1u << index);
    if (!live(e)) return LG_CONFLICT;
    if (!(e->command.targets & bit) || ((e->verified | e->failed) & bit) ||
        (!(e->sent & bit) && (verified || observed))) return LG_BAD_ARGUMENT;
    error = recheck(c, e, now);
    if (error != LG_OK) { cancel(c, e, error); return error; }
    bool matched = observed && matches(&e->command.intent, observed);
    if (observed) {
        merge_intent(&c->lights[index].observed, observed);
        c->lights[index].observed_ms = now;
        c->lights[index].observed_valid = true;
    }
    if (verified && matched) e->verified |= bit; else e->failed |= bit;
    c->lights[index].pending = false;
    c->lights[index].last_result = verified && matched ? LG_VERIFIED :
                                  (e->sent & bit) ? LG_UNKNOWN : LG_REJECTED;
    if ((uint8_t)(e->verified | e->failed) == e->command.targets) {
        e->result = !e->failed ? LG_VERIFIED : e->verified ? LG_PARTIAL :
                    e->sent ? LG_UNKNOWN : LG_REJECTED;
        e->error = e->failed ? LG_CONFLICT : LG_OK;
    }
    return verified && !matched ? LG_CONFLICT : LG_OK;
}

lg_error_t lg_observe(lg_core_t *c, uint8_t index, uint32_t expected,
                     const lg_intent_t *observed, uint64_t now) {
    if (index >= LG_MAX_LIGHTS || !intent_valid(observed)) return LG_BAD_ARGUMENT;
    lg_error_t error = lg_tick(c, now);
    if (error != LG_OK) return error;
    if (!(c->enabled & (1u << index))) return LG_BAD_ARGUMENT;
    lg_light_t *light = &c->lights[index];
    if (light->revision != expected) return LG_CONFLICT;
    for (size_t i = 0; i < LG_LEDGER_COUNT; ++i)
        if (live(&c->ledger[i]) && (c->ledger[i].command.targets & (1u << index))) return LG_CONFLICT;
    lg_intent_t merged = light->observed;
    merge_intent(&merged, observed);
    bool changed = !light->observed_valid || !intent_equal(&merged, &light->observed);
    if (changed && light->revision == UINT32_MAX) return LG_COUNTER_EXHAUSTED;
    light->observed = merged; light->observed_ms = now; light->observed_valid = true;
    if (changed) {
        light->revision++;
        light->last_result = LG_EMPTY;
        invalidate_overlap(c, (uint8_t)(1u << index), NULL);
    }
    return LG_OK;
}

static uint32_t crc32(const uint8_t *p, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static void put(uint8_t **cursor, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) { *(*cursor)++ = (uint8_t)value; value >>= 8; }
}

static uint64_t get(const uint8_t **cursor, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)*(*cursor)++ << (8u * i);
    return value;
}

lg_error_t lg_policy_encode(const lg_policy_t *p, uint8_t *out, size_t capacity, size_t *written) {
    if (written) *written = 0;
    if (!out || !written || capacity < LG_POLICY_WIRE_SIZE || !policy_valid(p)) return LG_BAD_ARGUMENT;
    uint8_t *cursor = out;
    put(&cursor, UINT32_C(0x314c4750), 4); /* PGL1, little endian. */
    put(&cursor, LG_POLICY_VERSION, 2); put(&cursor, LG_POLICY_WIRE_SIZE, 2);
    put(&cursor, p->epoch, 8); put(&cursor, p->guest_epoch, 8);
    put(&cursor, p->next_guest_id, 8); put(&cursor, p->clock_boot_id, 8);
    for (size_t i = 0; i < LG_MAX_LIGHTS; ++i) put(&cursor, p->freeze_until_ms[i], 8);
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        const lg_guest_t *g = &p->guests[i];
        put(&cursor, g->id, 8); put(&cursor, g->epoch, 8); put(&cursor, g->boot_id, 8);
        put(&cursor, g->expires_ms, 8); put(&cursor, g->revoked, 1);
    }
    put(&cursor, crc32(out, LG_POLICY_WIRE_SIZE - 4), 4);
    *written = (size_t)(cursor - out);
    return *written == LG_POLICY_WIRE_SIZE ? LG_OK : LG_BAD_ARGUMENT;
}

lg_error_t lg_policy_decode(lg_policy_t *p, const uint8_t *data, size_t size) {
    if (!p) return LG_BAD_ARGUMENT;
    memset(p, 0, sizeof(*p));
    if (!data || size != LG_POLICY_WIRE_SIZE) return LG_CORRUPT_POLICY;
    const uint8_t *tail = data + size - 4;
    if (get(&tail, 4) != crc32(data, size - 4)) return LG_CORRUPT_POLICY;
    const uint8_t *cursor = data;
    if (get(&cursor, 4) != UINT32_C(0x314c4750) || get(&cursor, 2) != LG_POLICY_VERSION ||
        get(&cursor, 2) != LG_POLICY_WIRE_SIZE) return LG_CORRUPT_POLICY;
    lg_policy_t decoded = {0};
    decoded.epoch = get(&cursor, 8); decoded.guest_epoch = get(&cursor, 8);
    decoded.next_guest_id = get(&cursor, 8); decoded.clock_boot_id = get(&cursor, 8);
    for (size_t i = 0; i < LG_MAX_LIGHTS; ++i) decoded.freeze_until_ms[i] = get(&cursor, 8);
    for (size_t i = 0; i < LG_MAX_GUESTS; ++i) {
        lg_guest_t *g = &decoded.guests[i];
        g->id = get(&cursor, 8); g->epoch = get(&cursor, 8); g->boot_id = get(&cursor, 8);
        g->expires_ms = get(&cursor, 8); g->revoked = (uint8_t)get(&cursor, 1);
    }
    if (!policy_valid(&decoded)) return LG_CORRUPT_POLICY;
    *p = decoded;
    return LG_OK;
}

const char *lg_error_name(lg_error_t error) {
    static const char *const names[] = {
        "ok", "duplicate", "idle", "bad_argument", "not_ready", "unauthorized",
        "revoked", "expired", "wrong_boot", "policy_changed", "stale_sequence",
        "conflict", "fresh_read_required", "frozen", "owner_hold", "overloaded",
        "id_reuse", "clock_error", "counter_exhausted", "corrupt_policy",
        "stale_stage", "not_found"
    };
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown_error";
}
