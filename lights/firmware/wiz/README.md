# WiZ local protocol and ESP-IDF UDP adapter

This isolated component implements the bulb-facing part of the lighting gateway.
It is not linked into the display application by its own CMake file and starts no
tasks, Wi-Fi interfaces, sockets, discovery, pairing, or light changes on load.
It has no dependency on the display or animation code.

## Source and supported subset

The protocol oracle is [pywizlight at commit
19e607afe0eb6db1473602639e194d691234d4fd](https://github.com/sbidy/pywizlight/tree/19e607afe0eb6db1473602639e194d691234d4fd),
specifically `utils.sign_request`, `PilotBuilder`/`PilotParser`, the module
fixtures, minimum-dimming logic, and scene class lists. Protocol behavior and
profile data adapted from that MIT-licensed project are attributed in
`LICENSE-pywizlight.txt`. The C implementation is native, with no Python runtime.

Production and host tests compile the repository's vendored cJSON and actual
mbedTLS/PSA implementation from ESP-IDF v6.1
(`fff9895c82d744c7237be8847347bdd1b07c6643`). There are no replacement crypto
stubs. Host-only ESP-IDF platform wrapper headers are reused from the existing
TLS tests; they do not replace cryptographic functions.

The method allowlist is `getPilot`, `getSystemConfig`, `getModelConfig`,
`getUserConfig`, `getPower`, and absolute `setPilot`. Supported writes are power,
brightness, raw RGB channels, Kelvin temperature, listed scene, and scene speed.
No raw JSON passthrough, toggle, delta, pulse, reset, reboot, flash, or blink is
exposed. Raw RGB values are channel commands; this component makes no colorimetric
or perceptual-conversion claim.

Capability discovery uses separate system/model/user/pilot replies. Exact module
names from upstream fixtures select conservative RGB, tunable-white,
dimmable-white, or socket profiles. No wildcard `RGB` substring matching or
unknown-model fallback grants broad capabilities. CCT controls require a reported
1000–10000 K range in addition to a supported profile. Module/firmware changes
invalidate previous capability evidence. Unknown devices may expose power and
brightness only when those fields are actually observed. Evidence flags distinguish
reported fields from curated class profiles; none is a cryptographic device claim.

Brightness is wire percent, **1–100**, with the actual `minDimLevel` if present and
upstream's conservative **10** fallback otherwise. A request below the minimum is
rejected, not silently clamped after authorization. The UI must normalize before
the authority accepts the command. A zero UI slider is an explicit power-off patch,
not `dimming:0`. Off cannot be combined with a style or brightness write. RGB, CCT,
and scene are exclusive. Speed is 10–200 and requires an explicit supported scene.
Custom modes, rhythm, sync-box modes, dual-head ratio/zones, white-channel mixing,
fans, schedules, power-on behavior, and onboarding are not implemented here.
Do not advertise full WiZ accessory parity from these capabilities.

## Signing and trust

A home key is exactly 16 bytes, imported from exactly 32 hexadecimal digits.
When the key is present or signing is required, every request is signed; a missing
key or trusted wall clock stops the operation. An error, timeout, or authentication
failure never causes an unsigned retry. Legacy unsigned control requires an
explicit decision by the owner-facing service before enrollment.

The compact JSON `params` bytes are produced once in a deterministic order,
followed by integer `sigTs`; **those exact bytes** are input to HMAC-SHA256 and
embedded unchanged in the packet. The method is deliberately not added to the
HMAC input, matching upstream. The known-answer test is:

```text
key hex: 00112233445566778899aabbccddeeff
params: {"state":true,"sigTs":1790441723}
HMAC base64: vARMv4JpVsxYabcBovABd25ISBzYT8+71v0CUtpU1sg=
```

The gateway must supply trusted synchronized Unix time and refresh it after clock
loss. Monotonic operation deadlines are separate from wall-clock signing time.
This code checks a broad 2020–2100 plausibility range; it cannot establish clock
trust or the model-specific acceptance window itself.

Replies remain **unverified local-network evidence**. Upstream does not verify
response HMACs, and this implementation does not invent a universal response
signature contract. The presence of `env:"pro"` or `hmac` is not authenticated-bulb
evidence. WiZ replies have no request nonce that this subset can reliably use.
Fresh sockets, matching method, expected source endpoint, enrolled MAC when
reported, and fresh association/DHCP checks limit stale/misdirected replies;
they do not make hostile local packet spoofing cryptographically impossible.
Keep the bulb AP private and gate the BLE/Internet gateway with the separate
authenticated authority service. Keys are never included in diagnostics or logs;
there are no logging calls in this component.

## Bounds and operation semantics

- At most eight explicitly enrolled MACs. Enrollment/removal requires the trusted
  owner service and is rejected while an operation is active. No remote client
  supplies an IP, port, raw method, or datagram.
- One operation and one socket at a time. Absolute writes and reads are the only
  retryable requests: at most three sends per phase at 300 ms, inside the original
  maximum two-second deadline. Reconnect, failover, and retry never extend it.
- An acknowledged write triggers a separate `getPilot`. Lost acknowledgements
  also progress to bounded readback. `write_sent`, `acknowledged`, `observed`, and
  `matches` remain separate. A mismatched observation returns `unconfirmed`; an
  interrupted write can remain unknown. Neither successful `sendto` nor an ACK is
  used as proof of the final state. A nonmatching read is retried within the same
  deadline (at most three reads) to let ordinary fades settle, without reapplying
  the acknowledged write. The latest actual observation is retained on failure.
- Authorization is rechecked while a write is pending and immediately before
  every physical command/readback send. Revocation, freeze, owner precedence, or
  expiry prevents further sends; it cannot undo a datagram already transmitted.
  The transport samples monotonic time **after** association/DHCP resolution,
  calls `wiz_send_guard_check`, and immediately invokes nonblocking `sendto`.
  This fences the syscall boundary; it does not promise RF delivery or bulb
  execution before the deadline. The original deadline is never extended.
- Input/output datagrams are capped at **2048 bytes**. Oversized model configuration
  is an explicit unsupported-size result, not a truncated document or a request
  to increase heap allocations. Before cJSON allocates, preflight limits nesting
  to six and structural tokens to 192. Duplicate decoded keys, embedded NULs,
  malformed/ambiguous responses, partial RGB triples, and out-of-range known
  fields are rejected. At most four incoming packets are handled per poll.
- The driver owns one shared 2049-byte transmit/receive buffer and eight slots,
  with a compile-time 4 KiB struct ceiling. Each received reply is fully parsed
  and copied before the buffer is reused; every retry re-encodes its request.
  I/O callbacks are synchronous and cannot retain the buffer. The ESP adapter
  receives directly into it: byte 2049 detects oversized datagrams, while the
  accepted wire limit remains 2048. cJSON's temporary
  heap allocation is bounded by the preflight input limits, but is not an arena;
  allocation failure returns malformed instead of silently accepting partial data.
  PSA also has its ordinary short-lived key/crypto allocation costs. Measure real
  free heap, stack watermark, and coexistence under the complete firmware before
  shipping.

## ESP-IDF integration

`wiz_esp_idf_init(adapter, ap_netif, authorize, context, &io)` creates an I/O
interface without changing Wi-Fi. `wiz_driver_init(driver, &io)` initializes the
engine. All calls run in one serialized worker; poll every 20–50 ms while active.
Never call from a GATT interrupt or rendering callback.

The supplied netif must be the private default `WIFI_AP_DEF` Wi-Fi AP with its
DHCP server. A station, Ethernet, or custom-key interface is rejected. Discovery
returns associated AP station/DHCP candidates, not identified bulbs. Each enrolled
MAC must still be associated and have a DHCP lease before and during each request.
IP drift terminates the operation; the next fresh operation resolves the new lease.
Static-IP-only bulbs and bulbs solely on peer-isolated campus Wi-Fi are therefore
not supported by this adapter. The gateway's upstream Internet connection can be
separate, while bulbs remain on the gateway AP.

Sockets bind both `SO_BINDTODEVICE` and the AP's local IPv4 address, with a fresh
ephemeral source port and destination UDP **38899**. Failure to bind fails closed;
there is no fallback through the campus station or Internet. Destinations must be
private unicast hosts in the same AP subnet, never the AP itself, network address,
broadcast, or multicast. The adapter asks ESP-IDF for current AP associations and
DHCP leases and re-resolves just before `sendto`; it does not guess addresses from
ARP or accept external discovery packets.

The authority integration contract is:

1. Authenticate the owner/guest in the service, map stable bulb IDs to enrolled
   indices, and validate/normalize against the current capabilities.
2. Submit an absolute `lg_intent_t` to the authority. Copy its six selected fields
   explicitly into `wiz_patch_t`; do not cast structs or send client-provided JSON.
3. In `io.authorize`, call `lg_check_send(command_id,index,now,&current)`, require
   success, and compare every selected/canonical field with the desired patch.
   The physical transport must call `wiz_send_guard_check` with a fresh clock
   after all lease/socket preparation and immediately before every send. It sets
   `guard.transmitted` only when the complete datagram was accepted by the socket.
   Mark `lg_prepare_send` only after a successful actual write, using the guard's
   fresh timestamp; scheduling or preflight alone must never mark a write sent.
4. After polling, carry forward at least `driver.last_now_ms`, which can advance
   beyond the timestamp supplied at poll entry. Use that fresh time for authority
   completion, expiry, and scheduling. A final-send cancellation/timeout also
   advances this sample, preventing accidental backwards-clock checks.
5. On correlated completion, use `lg_complete_target` with actual readback and
   `matches`. Preserve unverified-LAN trust at the UI/API boundary. An ACK alone
   has no observed fields and must never mark a target verified.
6. For independent polls, call `lg_observe` only for idle targets at the captured
   revision; do not let a stale periodic read cancel a newer accepted command.

`wiz_driver_poll` returns `busy`, `idle`, or `ok` when a terminal completion is
available. The terminal outcome is in **`completion.error`**, not the poll result.
The ESP adapter owns no persistence: the gateway service must store owner-approved
enrollment and keys safely and reconstruct this driver after startup.

## Reproducible checks

From the repository root, with its pinned SDK already installed:

```sh
cmake -S lights/firmware/wiz/tests -B lights/firmware/wiz/.build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DWIZ_SANITIZERS=ON
cmake --build lights/firmware/wiz/.build --target wiz_checks wiz_fixture_checks -j 6
ctest --test-dir lights/firmware/wiz/.build --output-on-failure
lights/firmware/wiz/.build/wiz_checks
python3 lights/firmware/wiz/tests/oracle_fixtures.py
python3 lights/firmware/wiz/tests/build_idf.py
```

The last command builds a separate **compile/link-only** ESP32-C3 project using
the real ESP-IDF Wi-Fi/netif/lwIP APIs. It has no flash step. Dependencies are local;
the component registry is disabled. Build products are ignored beneath this
component, keeping the main firmware's source fingerprint free of build files.

Host coverage includes signing vectors and missing/wrong keys/clock; strict MACs;
model and CCT bounds; dimming conventions; unsupported scenes and mixed modes;
truncated, duplicate, oversized and corrupted JSON; 10,000 deterministic arbitrary
datagrams under AddressSanitizer/UBSan; expected source/method/identity; lost ACK;
readback mismatch; authorization revoked before retry; DHCP changes; deadlines;
expiry during final-send lease work; fresh authority timestamps; clock regression;
bounded packet floods; exact 2048-byte replies; 2049-byte valid-prefix rejection;
shared-buffer signed retries; and removal/unknown targets. A separate
AST-only fixture loader replays all 52 literal public system/model/user replies
from the pinned upstream checkout, excluding its two exception-class fixtures.
It never imports or runs the upstream emulator. The 1.38.0 fixtures exercise
their larger nested model configurations inside the explicit parser budget.

These are protocol, crypto, state-machine, and build checks. They do not establish
successful physical pairing, room-bulb compatibility, firmware timing under load,
or production connectivity. Hardware qualification and actual safe bulb controls
must be recorded separately before a complete release is claimed.
