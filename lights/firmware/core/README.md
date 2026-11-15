# Lighting authority core

This directory is an isolated, host-tested C11 library. It is **not linked into
the installed display firmware**. It contains no Wi-Fi, BLE, UDP, TLS, UI, NVS,
password verifier, signature verification or provisioning code. No production
device or light was changed to test it.

Its job is to arbitrate already-authenticated light-control intentions before a
separate driver performs physical writes. Both Internet and BLE adapters must
call the same instance, serialized by one gateway worker. Do not call it
concurrently or put parsing/cryptography inside a BLE callback.

## Trust and authentication contract

`lg_identity_t` is trusted adapter input, **never a decoded user-supplied role**.
The adapter first proves the connection's identity and key binding, then maps it
to the owner principal (`session_id=1`, epoch zero) or a durably admitted guest
grant. An arbitrary network client passing `session_id=1` to a careless adapter
would bypass authentication; the core does not pretend to solve that problem.

Guest grants have increasing 64-bit IDs, the current guest credential epoch,
the gateway boot ID and a monotonic expiry. Sixteen records are bounded storage;
revoked, expired or prior-boot slots may be reused, but their **IDs never are**.
The durable next-ID high-water mark preserves this property after record
reclamation. Epoch rotation revokes all prior grants. A revoked grant cannot
be reactivated by editing a staged record.

Guest admission is a trusted operation after current-epoch credential proof.
Only the privileged adapter opens the policy stage; it must not expose that
owner capability to the enrolling guest. New grants bind to a client key in
the surrounding authentication implementation. Those key bindings and the
guest password verifier must be committed atomically with policy changes.

A shared password is not a person identity. Revoking one guest grant prevents
that grant from controlling lights through either path. Someone who still
knows the password may enroll under a **new** ID. Rotate the password/epoch to
prevent that; do not advertise a permanent person ban. Grant records are
boot-bound, so after a reboot the adapter deliberately authenticates again and
issues a new grant. There is no same-ID resurrection. `lg_stage_renew_guest` supports the narrow
boot-resume case: after fresh proof of the current credential **and** the stored
unrevoked token, it assigns a new increasing ID in the same record slot. It
rejects revoked, missing, old-epoch and same-boot expired sessions. The adapter
atomically retains the token/name binding with the new ID. A stale owner revoke
must refresh if it names a pre-renewal ID; do not silently claim the newly resumed
record was revoked.

## Command contract

`lg_command_t` contains a nonzero 128-bit UUID, a canonical payload digest,
boot ID, policy epoch, sequence, monotonic expiry, target mask, per-target
expected revisions and an absolute intent. Absent properties and unused
revisions must be zero. RGB, color temperature and scene selection are mutually
exclusive modes. The core's broad numeric bounds are parser bounds; the WiZ
adapter must additionally enforce the enrolled bulb's actual capabilities.

The adapter supplies its canonical digest; the core does not implement hashing
or accept that digest as authentication. Deduplication compares both the digest
and **every actual command field**. Reusing an existing UUID with changed
payload, digest or identity fails. The same UUID/payload on the other transport
returns its current result without incrementing revisions or sending again.

Sequences increase per guest grant and for the owner principal. Read the
snapshot's `last_sequence` on reconnect and serialize commands in the native
client. A new command must exceed it. Once a retained ledger entry has aged
out, the original sequence still prevents replay. UUIDs must never be reused
for different operations, including after the bounded deduplication window.

`expires_ms` is in the trusted gateway monotonic clock domain. The relay's
device lease must be converted to its original absolute deadline; receiving a
late packet must not restart its TTL. Maximum admission TTL is 2,000 ms. This
is an immediate-control core, not a scheduler for delayed or long-running jobs.
No new physical send/retry is allowed at or after expiry. Sent-but-unresolved
operations become `unknown`; later independent observations can still update
the displayed bulb state. A lost UDP response never proves the bulb stayed
unchanged.

Call `lg_snapshot` before the first mutation and after revision/policy conflict.
The core marks the affected targets as needing this new authoritative snapshot;
guessing a new revision without reading it is rejected. This is a fresh API
state read, not a claim that every bulb was physically polled at that instant.
Each observed state carries its own timestamp and validity bit.

### Dispatch and readback

1. `lg_submit` validates and queues an intention. It changes **desired** state,
   not observed state. Per-target `pending` and `last_result` distinguish queued,
   superseded, expired and observed outcomes without leaving a canceled group
   appearing busy. Owner work gets reserved capacity and dispatch priority.
2. `lg_next` selects a live owner request first. A driver with one physical
   request in flight must not start another merely because `lg_next` is callable.
3. Call `lg_check_send` immediately before **each** bulb send or retry. It
   rechecks the grant, policy, freeze, owner hold, target revision and expiry,
   without claiming that a packet left the gateway. In that same serialized
   worker, call `lg_prepare_send` directly after the UDP socket accepts the send
   to record it. This send marker is not delivery or readback proof.
4. Call `lg_complete_target` with supported, correlated readback. `verified=true`
   alone is insufficient: the observed requested properties must also match.
   UDP send success or an acknowledgement without readback is not verification.
   Complete an encoding/key/socket failure before any send with `NULL,false`;
   it becomes `rejected` with an empty sent mask, not a fictitious physical send.
5. Use `lg_observe` only for an independent, fresh poll of an **idle** target at
   its captured revision. A poll racing an active command is rejected, as is a
   response to an older revision. This prevents polling the pre-write bulb state
   from canceling a just-accepted user action.

Guest group admission is atomic: one locked target rejects the whole command.
Physical fan-out is not atomic. Partial observations produce `partial`; failed
or unobserved changes produce `unknown`. The core does not roll successful bulbs
back after partial failure. An owner command supersedes overlapping queued
intentions; an already-sent packet cannot be recalled. Late readback from an
older intention cannot overwrite a newer owner's state.

The API's readback verification is not a claim of cryptographic response
authentication or physical sensing. The driver must separately establish the
bulb identity and report the available security guarantees. In particular,
unsigned legacy WiZ control on a guest-known AP can bypass this entire core.

## Owner policies and persistence

Owner writes may request a hold of at most 60 seconds. Guests cannot create,
extend or release these holds. Reads and heartbeats do not extend them. Holds
are transient command arbitration and clear on reboot; use a persisted freeze
when the restriction must survive reboot. `lg_clear_owner_holds` releases them
early for the authenticated owner.

Freezes cover a target/group mask, or all eight bits for the room (including
future slots). They may be timed, indefinite (`UINT64_MAX`) or cleared (zero).
They survive in the policy record. A freeze from a different boot remains
effective conservatively until the owner resolves it; elapsed wall time cannot
be reconstructed from a reset monotonic clock.

Policy updates deliberately use an explicit staging copy:

```c
lg_policy_stage_t stage;
lg_policy_stage_begin(core, authenticated_owner, &stage, now_ms);
lg_stage_freeze(&stage, targets, now_ms, duration_ms);
lg_policy_encode(&stage.policy, record, sizeof(record), &record_size);
/* Integration: atomically commit record + associated auth changes, read back. */
lg_policy_activate_after_commit(core, &stage, current_monotonic_ms);
/* Only now return "applied". Any earlier failure leaves old active policy. */
```

Check every return value; the example omits error branches only for brevity.
`lg_policy_activate_after_commit` does not write storage and cannot verify that
the caller did. Its name/API makes the boundary explicit. Serialize the entire
stage/write/activate transaction, or use a storage compare-and-swap against
`base_epoch`. Do not overwrite durable storage with a stale stage and then rely
on the core rejecting activation. If a crash follows the durable commit but
precedes activation, reboot loads the new committed policy.

The 636-byte little-endian record has a magic, version, length and CRC32. It is
not a raw struct dump. CRC catches accidental corruption, **not malicious
modification**. Select the newest committed valid copy in the integration's
copy-on-write storage; fail closed if no valid record exists. Decode errors
zero the output, and `lg_init` with that invalid policy leaves the core not
ready. Never silently invoke `lg_policy_factory` after decode failure. Factory
creation is for explicit initial/recovery provisioning with fresh credentials.

Adding/removing bulbs must not reinitialize the live core: that would discard
sequence/deduplication state in the same boot. Use:

1. `lg_validate_enabled_change` for owner authorization and counter preflight.
2. Atomically persist the gateway's bulb registry, without interleaved worker
   mutations.
3. `lg_set_enabled_after_commit` to apply the same mask.

Changed slots advance their revisions, invalidate affected commands and clear
old desired/observed state and temporary holds. Other slots, grants and ledger
history survive. Persistent freeze bits remain conservative on reused slots;
the owner can explicitly thaw them. The empty-room mask zero is supported.
`lg_snapshot(..., targets=0, ...)` reads room metadata without claiming to have
refreshed any target.

## Bounds, validation and remaining integration gates

| Item | Fixed limit / measured size |
|---|---:|
| Registered target slots | 8; installed product profile must limit enrollment to 5 |
| Guest grant records | 16 |
| Ledger entries | 32, including 4 unavailable to guest requests |
| Entry lifetime | Original expiry plus 1,000 ms; never evict an unexpired/live record |
| Core state, host and ESP RISC-V ABI | 7,248 bytes |
| Policy / staging copy | 736 / 768 bytes |
| Encoded policy | 636 bytes |
| Allocations by this core | None |

All capacity failures return errors/backpressure. Guest floods cannot use the
four owner-reserved ledger entries. These are not complete network DoS controls:
the gateway/relay also need admission throttles, quotas, bounded authenticated
session pools and BLE handshake deadlines. Observe no-progress connection
timeouts independently of this command core.

The core's static assertion limits it to 8 KiB; stage buffers, serialization,
snapshots, driver, crypto, transport and worker stack are **additional** memory.
Keep staging and serialization off the high-priority BLE callback stack. This
code fitting in C does not establish that the whole C3 application fits its
runtime RAM/radio budget. See the [gateway feasibility report](../../../docs/lights/gateway-architecture.md).

Run the host suite:

```sh
make -C lights/firmware/core test
```

The recorded run passed **7,838 assertions** with AddressSanitizer and
UndefinedBehaviorSanitizer, strict C11 warnings and no failures. It exercises
dual-path duplicates, altered-payload IDs, out-of-order requests, conflicts,
owner priority, holds, group freezes, persistence staging, individual revocation,
epoch rotation, grant-slot churn, resets, expiry, partial readback, stale polling,
overload reservations, empty-room onboarding, live registry replacement, pre-send failures and narrowly
authorized boot-only grant renewal.
The persistence tests reject every one-bit corruption of the 636-byte record,
every truncated length, unsupported version and selected structurally invalid
records with otherwise correct CRCs.

The installed ESP toolchain also compiled this file for `rv32imc/ilp32` with
`-Os -Wall -Wextra -Wconversion -Werror -pedantic`. The standalone object totals 9,562 bytes of text/constant data before final
linking. The undefined symbols are
only compiler shift support and `memcmp`/`memcpy`/`memset`; no heap routines.
This is a compile check, not physical device execution. Transport authentication,
NVS power-loss behavior, the actual WiZ fleet, display/game timing, combined
memory use and campus/BLE coexistence still require integration/hardware tests.
