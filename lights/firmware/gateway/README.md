# Bounded lighting gateway service

This component is an isolated, compileable integration candidate. It has not
been installed on the production display and its host tests use simulated UDP.
It does not modify Wi-Fi, add a TLS connection, or create a task by itself.

`gateway_service.h` is the worker-facing API. A single application worker owns
the service, calls `lgs_service_request()` only after Security2 and gateway
identity binding, and polls `lgs_service_tick()` every 20–50 ms. Bluetooth callbacks
and the existing relay task enqueue bounded work; they must not call this code
concurrently or perform SRP/UDP/NVS work in their callbacks.

Implemented paths:

- Owner provisioning through an existing trusted USB/management boundary;
  there is no public first-claim endpoint. Owner and guest SRP profiles are
  distinct from the WiZ home key and firmware signing key.
- Shared-password admission with per-install 256-bit tokens (only SHA-256 is
  persisted on the gateway), individually revocable grants, password epoch
  rotation, durable guest freezes and per-command owner precedence. Fresh
  current-password SRP plus an unrevoked remembered token renews the same
  installation after reboot under a new monotonically increasing grant ID.
- One bounded WiZ driver with reauthorization before every physical attempt,
  absolute commands, UUID/revision/sequence/boot/deadline checks, and correlated
  readback. Owner work preempts a stalled guest exchange or background read;
  preempted writes remain unknown when they may have reached a bulb.
- Capability-bounded power, brightness, RGB, white temperature, supported scene
  and scene speed. Unknown device features stay disabled. Unsupported native
  WiZ settings/rhythms are not silently simulated or reported as successful.
- Read-only AP/DHCP candidate enumeration followed by typed unicast probes,
  explicit owner adoption, model discovery, individual status and removal.
  Candidates are associated LAN peers with a matching reported MAC; their
  replies are not cryptographically authenticated. The installed-C3 product
  profile limits enrollment to five bulbs, with eight stable record slots.
- Paged JSON reads, typed errors, actual sent/queued/readback states, clock and
  driver failure details, and explicit legacy-control risk. No arbitrary UDP,
  destination, network credential, URL or raw WiZ-method tunnel exists.

See [PROTOCOL.md](PROTOCOL.md) for wire fields. The native app must remember a
guest password plus its individual token in non-synchronizing Keychain when
the user chooses Remember Access, because a token alone cannot recreate SRP
against the shared-password verifier. A revoked token must never trigger
automatic enrollment. Knowing the shared password still allows an explicit
new enrollment; a session revoke is not a permanent ban on a person.

The 24-hour grant TTL is monotonic **within one boot**, not an absolute 24-hour
wall-clock promise across power loss. Boot renewal requires fresh proof of the
current shared password and the same retained, unrevoked token. Same-boot expired,
revoked and old-password-epoch grants do not renew. Revoking an ID replaced by
renewal returns conflict so the owner refreshes the current session identity.

## Persistence and resource budget

The existing 32 KiB `state` partition stores the `lights_v1` namespace. There is no
partition-table change and no erase/cleanup fallback. Two 3072-byte versioned CRC
records plus a small atomic `head` key retain profiles, bulb registry, policy,
grant labels and token hashes. The record encoding is independent of C padding
and endianness. An update commits the inactive blob first, then its generation
head; only then does the service activate the policy in memory.

An orphan newer blob is ignored. A corrupt head-selected blob enters maintenance
instead of falling back to an older policy that could restore revoked access.
An ambiguous failed commit stops dispatch until recovery. Runtime must close
guest Security2/BLE connections on revocation, password rotation and failures.
Application route bindings additionally expire after 60 seconds without an
authenticated application request; this prevents permanent slot loss if a
disconnect event is lost. Every crypto teardown should still close its route
immediately.

NVS admission conservatively reserves 328 entries on initial provision and
228 entries on later writes, including a full 126-entry garbage-collection page.
This is approximately 10.5 KiB/7.3 KiB of free NVS entry capacity, respectively;
it is not an assumption that the shared partition currently has that room.
Existing OTA metadata and the optional 12 KiB media clip are never deleted to
make space. `nvs_get_stats()` checks the actual partition before writes.

Host `sizeof(lgs_service_t)` is about 18 KiB, including authority, WiZ driver,
record, bindings and candidate storage. The ESP UDP adapter and crypto/session
pool are additional. A policy write temporarily allocates a 3176-byte record and
3072-byte encoding; loading decodes a record on the stack. Start with at least an
8 KiB worker stack, measure stack watermarks, current free heap and largest block,
and guard persistence as well as SRP admission. This module cannot establish
that the C3's display/games/Bluetooth memory budget is safe.

## Reproducible checks

From the repository root:

```sh
cmake -S lights/firmware/gateway/tests -B .local/lights-gateway-checks -DWIZ_SANITIZERS=ON
cmake --build .local/lights-gateway-checks --target gateway_checks -j4
.local/lights-gateway-checks/gateway_checks
python3 lights/firmware/gateway/tests/build_idf.py
```

The host suite exercises all 24,576 single-bit corruptions of a stored record,
power-cut boundaries around the head commit, no older-policy fallback, typed
JSON/authentication checks, fake-UDP command acceptance/readback, duplicate
commands, durable revocation/freeze, failed-storage dispatch suppression,
read-only discovery/legacy opt-in, stale route reclamation, and prompt owner
preemption of an unresponsive guest target. It also covers boot renewal and
stale revoke races, ambiguous commit acknowledgements, five-bulb admission,
and a missing-clock failure that sends no UDP and reports `sent:0`. The separate pinned ESP-IDF 6.1
compile/link fixture performs no flashing.

Remaining hardware gates include actual bulb commissioning and mandatory
signature enforcement, WiZ model coverage, source-peer behavior on the AP,
time bootstrap, concurrent Internet/BLE load, five bulbs, display/game timing,
heap/stack/flash margins, OTA regression and the 60-minute combined soak. A home
key being present is not evidence that bulbs reject unsigned control.
