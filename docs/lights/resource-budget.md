# Same-C3 lighting resource audit

Measured 2026-11-16 with ESP-IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643`. This is a build and source audit,
not combined-device qualification. These audit actions did not flash hardware,
control a light, replace the retained production build or change the Mac's
network. Subsequent installation and live qualification are recorded separately;
this document's build flags are not a statement of the device's current state.

The latest measurement is the isolated, production-key-signed **0.3.0-rc.16**
hardware-qualification build. It includes the local time service, bounded
20-socket profile, stack metrics and abnormal-reset lighting quarantine. Earlier
disposable-key measurements remain below as historical snapshots.

## Decision

There is a concrete candidate for keeping the installed C3: move selected Wi-Fi
and BLE paths from internal RAM to flash, use supported flash auto-suspend, avoid
duplicate TLS connections, and recover idle worker memory. The latest isolated
link containing the current display application, BLE transport, Security2
sessions, lighting authority, gateway persistence, WiZ driver and guarded runtime fits the
existing signed OTA slot at **1,708,032 bytes**, with **258,048 bytes remaining**.

That integrated link uses **129,269 bytes of unified internal static RAM**. It leaves
**192,027 bytes before runtime allocations**. This is 35,093 bytes less static RAM
than RC15. The complete 22,120 B runtime is allocated lazily; shared display/game
workspace and relay buffers also reduce static storage. BLE stacks/pools, authentication peaks, the new application worker,
TLS, Wi-Fi station state, JSON and storage transactions still consume heap.
The runtime, its 10,240 B worker and known BLE stack/MSYS lower bound together
consume at least **42,856 B** when active, before other radio/crypto/TLS allocations.
The separate three-entry lighting reply queue adds at least 3,156 B, for a
**46,012 B known lower bound**, before its queue header and those other allocations.
Consequently the candidate is **not hardware-qualified** and does not yet
prove that five bulbs plus BLE can run without affecting the display. The
numeric activation and qualification gates below intentionally fail closed.

## Baseline and evidence boundary

The retained RC15 signed image is 1,314,816 bytes. Its SHA-256 is
`d3b56b5910729c281fd3728612a07a2b6be8fae8b7417b9e3e259c1e01f9c3cf`, matching
`.local/animation-slot2/published/manifest.json`. Its ELF and map were also
unchanged after the experiments:

| Artifact | SHA-256 |
| --- | --- |
| `build/203-entrance.elf` | `2c4e4027fa5d60ba6ae6d70027bf37e53e1335c37deb2ac9eadd350fb2780652` |
| `build/203-entrance.map` | `3689e2b7702fa049bf38292eb703b7d41698cacdf6a49537952f604a08f75e89` |

`esp_idf_size` reports 164,362 bytes of RC15 unified static RAM: 75,672 B BSS,
14,140 B data and 74,550 B RAM-resident code. The reported internal region totals
321,296 B, leaving 156,934 B at link time. On this C3, RAM-resident instructions
and data compete for the same internal capacity. **Linker free bytes are not
free runtime heap.**

Historical installed-device evidence in `.local/animation-slot2/installed-summary.json`
and `final-health.json` records:

| Observation | Evidence |
| --- | --- |
| Installed version / animation | RC15 / slot 2; user physically confirmed movement and layout |
| Successful health snapshots | 11 over 133,898 ms |
| Sampled free heap | 45,236–64,220 B |
| Recorded lifetime minimum heap | 28,588 B |
| Largest free block in successful snapshots | 26,624–38,912 B |
| Management reads that failed | 5; weak hub link interrupted the continuous sequence |
| 60-minute soak | **Not completed** |

Render counters advanced by 1,641 while late-frame counters advanced by 227
(about 13.8%) in that recorded interval. Reported last-frame times were
54,729–66,971 µs; the lifetime maximum was 2,075,620 µs. The snapshots do not
identify when that maximum occurred or provide a percentile distribution.
These are baseline limitations, not regressions attributable to lighting.
They must not be described as a completed stability gate.

A subsequent point-in-time read through the USB maintenance hub, retained privately in
`.local/lights/production-baseline.json`, reported RC15 at uptime
6,787,468 ms: **55,872 B free**, **36,864 B largest block**, and **28,588 B
lifetime minimum**. This is one status snapshot from the production firmware,
not a lighting measurement, a client-free idle watermark or a soak. The active
display has no stored media clip. Keep this measurement separate from predicted
savings predicted from candidate build artifacts.

A later RC15 baseline attempt, recorded in
`.local/lights/rc15-baseline-soak.json`, has **148 retained samples** spanning
**2,398.952 seconds** against a 3,600-second requirement and reports
`passed: false`, `Stale or missing soak telemetry`. The host-run investigation
identified a **33.295-second login renewal** after the preceding **15-second
poll sleep**, exceeding the **45-second telemetry gate**. The rejected late
poll is not a retained sample, so the retained inter-sample gaps alone do not
show that terminal failure. This is a failed monitoring-continuity gate, not a
completed 60-minute soak or evidence attributing a reset to lighting. The host
scheduling correction passed seven soak-unit tests; a new continuous run is
still needed. Its status belongs in the separate validation record.
The failed artifact's SHA-256 at this audit snapshot was
`28dfa35597e446345b8530d896d522d0a0d8e297e0effbcbc0bc8acbbf989093`.

## Reproducible isolated measurements

The checked-in experiment is [the probe project](../../lights/firmware/probe/README.md).
It uses the pinned local SDK/toolchain and already resolved dependencies with
the component manager disabled. Build objects are reused between profiles.
The standalone and historical combined probes, plus the default-off regression,
use ignored, disposable RSA-3072 keys and cannot pass the installed device's
image trust check. RC16 was built separately with the existing protected
production key, explicitly for controlled hardware qualification; its signature
does not establish hardware reliability or a stable release.

The original combined probe snapshots application sources into
`.local/lights-resource-probe/combined/source`; it does not edit production
sources or `.tools`. Public API references keep complete code paths in the link.
The fixture is not a functioning gateway integration: its credentials are
deliberately invalid, it does not run handshakes or bulb commands, and it has no
finished runtime coordinator. Two extra raw Security2 contexts retained by the
fixture would add a small runtime cost if it were ever run; they do not represent
the intended final session architecture.

### Standalone component stages

These isolate link costs, including dependencies that production already has.
Do not add all stage deltas to the production application.

| Stage | Contents | Binary B | Unified static RAM B | Linker free B |
| --- | --- | ---: | ---: | ---: |
| 0 | NVS and heap logging | 143,408 | 47,697 | 273,599 |
| 1 | Minimal peripheral NimBLE service | 370,576 | 77,923 | 243,373 |
| 2 | Two raw Security2 contexts | 480,288 | 79,171 | 242,125 |
| 3 | Complete three-entry session-manager API | 495,792 | 80,337 | 240,959 |

NimBLE's standalone static increment is 30,226 B, but it includes PHY and other
code already present in RC15. New BT controller, host and coexistence archives
account for about 21.8 KB of genuinely additional static RAM before their heap
allocations. Security2's standalone flash increment also overstates its
increment in production because production already uses cryptography.

### Combined application profiles

| Profile | Signed image B | Unified static RAM B | Linker free B |
| --- | ---: | ---: | ---: |
| RC15 production baseline | 1,314,816 | 164,362 | 156,934 |
| Original Wi-Fi placement + minimal BLE + Security2 + authority | 1,576,960 | 196,709 | 124,587 |
| Wi-Fi hot paths in flash + NimBLE low-speed placement | 1,576,960 | 176,649 | 144,647 |
| Above + BLE controller in flash | 1,642,496 | 158,431 | 162,865 |
| Above + initial real bounded BLE transport | 1,642,496 | 162,935 | 158,361 |
| Above + flash auto-suspend + final transport wake/fairness changes | 1,642,496 | 163,013 | 158,283 |
| Snapshot: above + gateway, NVS and WiZ driver reserved statically | 1,708,032 | 174,325 | 146,971 |
| Snapshot: integrated root candidate with guarded runtime allocated lazily | 1,708,032 | 153,381 | 167,915 |
| Snapshot: display/relay/WiZ memory changes, before recovery diagnostics | 1,708,032 | 128,869 | 192,427 |
| Snapshot: disposable-key candidate + bounded inactive-runtime recovery and diagnostics | 1,708,032 | 128,949 | 192,347 |
| Last default-off full regression, separate disposable key | 1,314,816 | 164,362 | 156,934 |
| **RC16 hardware candidate:** local time service, 20 sockets, stack metrics and reset quarantine; production key | **1,708,032** | **129,269** | **192,027** |

Each row is a specific snapshot. The gateway snapshot's source hashes, artifact hashes,
map, resolved configuration and build log are retained under
`.local/lights-resource-probe/combined/wifi-bt-suspend-transport-gateway-*`.
The preceding gateway snapshot did not include the final runtime coordinator.
The disposable-key runtime snapshot is the actual root project with
`ENTRANCE_LIGHTS=ON`, built through
`lights/tools/build_candidate.py`. Its source digest, resolved profile, size
reports, signature check and unchanged-production-artifact evidence are in
`.local/lights-candidate/summary.json`. Its signed image SHA-256 is
`847fadfef9c7a535cd78d7f97171b911e03d146d0a5fb17a3ccd32fa53715cca`.
Its ELF SHA-256 is `9dd77077161ae5fbcc7f7bbb2cae9dfe1c26ad9cd9e22e5761567bcb1a031c03`.
The separate default-off evidence is in `.local/lights-default-off/summary.json`;
its static RAM exactly matches the retained baseline, with Bluetooth disabled
and 16 sockets. Its signed image SHA-256 is
`86460e501dfc65ebf0f4cfe316ff6e586c6bf287b8022eb8380f5cc032472313`.
That full default-off build predates the final RC16 source checkpoint; it is not
presented as a same-revision enabled/disabled comparison. All six protected
default-off files and the retained production files were verified unchanged by
the later RC16 build.
Differences between snapshots include actual source revisions as well as
storage placement; do not describe them all as configuration-only deltas.

### RC16 provenance and signature audit

The hardware candidate used the clean source commit
`54bdb752c6e5c6d805647809461a839241fccfc7`, tree
`658b048930245f6b6ad6926fab5359c407fe38fc`, with no tag required for this local
qualification. Its source-input SHA-256 is
`282574a626339a6d8103ec1ed1516204a5d182fc87498c533fd04a060dd6813c`.
The build completed at `2026-11-15T23:00:19Z` (2026-11-16 locally). Files are
under `.local/lights-hardware/0.3.0-rc.16/`:

| Artifact | SHA-256 |
| --- | --- |
| `build/203-entrance.bin` | `5d3ee49dc711360821b9dd74cd1bf160146da2bfd773cb7e9287b3a9e745ee8c` |
| `build/203-entrance.elf` | `2a5d014265bd9dbcabb5042302d646fcf7b47ea027c0efbffa7c09d76a7304f2` |
| `build/203-entrance.map` | `68e15a9e234bb9bef5ef3733fc38f7293e46cbdd929a759ed7b68e97a40b4bec` |
| `sdkconfig` | `b542aa38d11c297941b9497fba0512af4d674446ab120840fa7162337fa3e1ea` |
| `summary.json` | `f951607cf265c8620c94f152121f1593a47c69d7fd0901cf30406e06801c6422` |
| `signature-block-audit.json` | `16e51448132c631767b228dce8b4d8cff3fd9479e442f2a7ba5c0037336f1fb2` |

The builder verified the baseline and candidate using the production public key,
SDK signing/rollback configuration, sensitive-log compilation caps, stable
source/dependency fingerprints and before/after preservation of production
artifacts and signing files. The output directory is mode 0700; provenance is mode 0600.
No private key was generated, replaced or copied.

An independent read-only audit using pinned `espsecure` 5.6.0 checked **all three
signature slots**, rather than stopping at the first absent slot. Exactly block
0 is valid RSA-3072; its complete RSA verification primitives match the
production public key, and its image digest matches the signed content. The
remaining **2,880 bytes** of the signature sector are all erased (`0xff`), so
there is no second signature block extending image trust. Recorded public-key
digests are:

- Canonical public DER SHA-256:
  `2da2432cc268727f4dfbffb42fa9aba686a5a24ef9b514a1abd9e34915cd60d1`.
- ESP Secure Boot key-format digest:
  `6be30c92b1b84c8bef68979259645902e8fa9f86ed4b59afdd2cf0c31469267c`.

These are public-key fingerprints, not private credentials. No eFuse operation,
installation, provisioning or publication formed part of this build/audit.

RC16's static total consists of **57,144 B BSS**, **15,361 B data** and
**56,764 B RAM-resident code**. Compared with the prior 128,949 B candidate, the
increase is **320 B**, accounted for by linked symbols/archive measurements:

| Addition | Static bytes |
| --- | ---: |
| Four socket entries | 80 |
| IPv4 multicast membership entries | 48 |
| IPv6 multicast membership entries | 112 |
| Received-time anchor | 24 |
| Runtime status, stack sampler and boot quarantine fields | 51 |
| Link alignment | 5 |

The socket limit's measured static cost is therefore **240 B**. This excludes
heap allocations for live sockets, TLS, packet queues and actual traffic. The
local time helper separately adds **504 B** to the lazily allocated runtime
context; it does not create another task or stack.

Turning off the two Wi-Fi IRAM optimizations saved 19,732 B across the Wi-Fi
archives in this target. NimBLE low-speed placement saved a further 182 B in
its host archive; total linked change was 20,060 B. Moving the controller to
flash then saved 18,218 B. Generic Kconfig estimates must not be added together
as independent guaranteed savings. The actual final transport added about
4.5 KB, including its caller's 1,072 B receive-event object.

## Candidate configuration and flash constraint

The candidate uses peripheral-only NimBLE, two BLE connections, three controller
activities, preferred ATT MTU 128, a 4,096 B host stack, six 128 B and six 256 B
MSYS blocks, six 128 B controller ACL buffers, and ten ordinary plus two
discardable event buffers. Central, observer, GATT client, Bluetooth 5 extras and
SMP are disabled. Application authentication/encryption is Security2. A bounded
fragment protocol handles messages larger than ATT capacity; MTU 128 does not
mean a 384 B SRP public key fits in one ATT write. These pool counts need physical
flow-control and two-client tests.

The isolated candidate additionally sets:

```text
CONFIG_ESP_WIFI_IRAM_OPT=n
CONFIG_ESP_WIFI_RX_IRAM_OPT=n
CONFIG_BT_NIMBLE_LOW_SPEED_MODE=y
CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y
CONFIG_SPI_FLASH_AUTO_SUSPEND=y
CONFIG_SPI_FLASH_ROM_IMPL=n
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
CONFIG_LWIP_MAX_SOCKETS=20
```

RC16 resolves `CONFIG_LWIP_MAX_SOCKETS=20`, checked by the builder. The combined
management listeners/clients, relay, WiZ, AP DNS and NTP
require at least 18 slots including release headroom. The service refuses an
insufficient configured budget; default-off retains 16. This bounded increase
does not claim that 20 simultaneous sockets are a qualified heap workload.

These are supported controls with explicit tradeoffs. Wi-Fi throughput and BLE
timing may decrease when code moves to flash. The controller Kconfig specifically
warns about operation during flash erasure and recommends auto-suspend. The
intermediate controller-in-flash row **without** auto-suspend is a measurement,
not a deployable profile. [Pinned Wi-Fi Kconfig](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/esp_wifi/Kconfig),
[NimBLE Kconfig](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/bt/host/nimble/Kconfig.in),
[C3 controller Kconfig](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/bt/controller/esp32c3/Kconfig.in).

The retained flash-identification record for this installed board reports JEDEC
manufacturer `0x46`, device `0x4016`, 4 MB embedded XMC flash. The pinned generic
flash driver explicitly classifies manufacturer `0x46` as XMC-D with suspend
capability. This gives a hardware-specific basis for the auto-suspend candidate.
It is still not a reliability test. The installed IDF 6.1 bootloader satisfies
the documented post-4.3 bootloader prerequisite. No bootloader replacement is
proposed. Do not enable the separate force-XMC-C-suspend option. [Pinned flash
capability implementation](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/spi_flash/spi_flash_chip_generic.c),
[flash Kconfig](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/spi_flash/Kconfig),
[supported flash-feature documentation](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/docs/en/api-reference/peripherals/spi_flash/spi_flash_optional_feature.rst).

`ESP_PHY_IRAM_OPT` is not an additional C3 saving here: the pinned option affects
ESP32-C2. Releasing controller memory after shutdown is not a saving for an
active, persistent BLE service. No stack has been shrunk based only on its
nominal size.

## Runtime allocations not established by the linker

Runtime/service sizes below were read from the RC16 C3 ELF's DWARF; unchanged
authority and SDK internals retain their earlier C3 measurements. None are
inferred from macOS ABI:

| Object | C3 bytes | Accounting |
| --- | ---: | --- |
| `lg_core_t` | 7,248 | Included in `lgs_service_t` |
| `lg_policy_t` | 736 | Also represented in persistent record |
| `lg_policy_stage_t` | 768 | Temporary policy transaction |
| `lgs_record_t` | 3,176 | Included in service; a transaction can duplicate it |
| `wiz_driver_t` | 3,896 | Included in service; shared transmit/receive storage |
| `lgs_service_t` | 16,072 | Entire service, including authority/record/driver |
| `wiz_esp_idf_t` | 16 | Socket adapter receives into bounded caller storage |
| `ls_manager_t` | 1,040 | Three secure-session entries, excluding allocated crypto |
| `lb_event_t` | 1,072 | Caller-owned BLE event buffer |
| `lrq_t` | 3,384 | Bounded relay queue inside runtime |
| `runtime_t` | 22,120 | Complete retained service/session/adapter/queue/input/local-time aggregate |
| `light_reply_t` | 1,052 | Three separate queued replies, excluding queue metadata |
| SDK Security2 `session_t` | 60 | Excludes SRP/PSA allocations |
| `esp_srp_handle` / `mbedtls_mpi` | 80 / 8 | Exclude big-number limbs |

The earlier gateway snapshot reserved service and adapter statically, totaling
20,060 B at that revision. RC16 allocates its complete **22,120 B** aggregate on
activation, 504 B more than the preceding 21,616 B runtime. Lazy allocation reduces dormant memory;
it does not reduce active memory. Do not keep the standalone authority
object as well. A policy write additionally allocates a 3,176 B record and a
3,072 B encoded buffer, before NVS, JSON and crypto overhead.

Two BLE task stacks alone consume 8,192 B: 4,096 B host and 4,096 B controller.
The configured MSYS payload blocks contribute at least 2,304 B. Thus these
known items alone require **at least 10,496 B**, before task control blocks,
allocator headers, packet headers, connection state, event/ACL pools, controller
internals or application worker. That is a lower bound, not a complete BLE heap
estimate. The controller's activity count and NimBLE pool limits are allocation
limits, not proof that peak packet traffic fits them.

The current SDK retains roughly **3 KB per established Security2 session** in
SRP state before allocator overhead: 3072-bit operands/cache, serialized A/B,
salt, 64 B session key, small structs and username. This is a source-based
estimate; limb capacities may exceed mathematical lengths. Three established
sessions can therefore retain about 9 KB unnecessarily after authentication.
SRP's transient peak is larger and has not been measured. C3 hardware MPI is
enabled, so generic software exponentiation-window memory estimates are not
reliable substitutes for instrumentation.

PSA key slots, transcript hashing, protobuf, cJSON, input/output copies, TLS
handshakes, Wi-Fi peers and NVS are further variable allocations. RC15's HTTPS
server permits three sockets and has no shared peak-allocation reservation.
The integrated candidate now wraps new synchronous client/server TLS handshakes
with the same reservation used by SRP/activation. That integration has linked
but has not been exercised on hardware. Existing TLS sessions still retain
buffers, and the lighting relay reply queue allocates separately from `runtime_t`.
A heap precheck alone is not a reservation. [Espressif RAM measurement guidance](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-guides/performance/ram-usage.html).

### Security2 post-handshake cleanup review

No SDK fork was implemented. A narrowly scoped, separately vendored Security2
implementation could release retained SRP state after successful CMD1 without
changing the wire protocol, but must be reviewed and tested first:

1. `cur_session->session_key` aliases memory owned by `srp_hd`. It must not be
   freed independently. Salt/verifier fields reference externally owned profile
   storage and must not be freed.
2. `psa_import_key` imports the first 32 key bytes. Once proof verification and
   import succeed, AES-GCM uses the PSA key ID and the session IV/counter; it
   does not require the SRP context or username.
3. The conservative cleanup point is after successful response packing and
   `sec2_session_setup_cleanup` for CMD1 in DONE state. The response must first
   own its packed proof/nonce bytes. Zeroize the aliased key, call
   `esp_srp_free`, then clear `srp_hd`, `session_key` and length; wipe/free/null
   the username. Preserve the PSA key ID, IV, state and session ID.
4. Verify close, retry/rehandshake, invalid proof, timeout, every allocation
   failure, response-allocation failure and repeated concurrent-context tests.
   Existing error paths must not gain a double-free, leak or stale alias. A
   response-allocation failure currently merits explicit cleanup review because
   it can return before the SDK response-cleanup call.

The actual SDK crypto log tag is `srp6a`. Both `protocomm` and `esp_security`
were compiled with `LOG_LOCAL_LEVEL=ESP_LOG_INFO`; compile commands were checked.
Runtime log suppression alone does not remove the SDK's DEBUG key/proof dumps.
Even with logs compiled out, `hexdump_mpi` still contains temporary serialization
allocation, which belongs in a peak-memory audit. [Pinned Security2 source](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/protocomm/src/security/security2.c),
[SRP ownership implementation](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/protocomm/src/crypto/srp6a/esp_srp.c).

## Implemented savings and remaining options

The enabled candidate now includes these changes. Object/link measurements
establish their storage cost, and host tests cover the stated behavior. They do
not establish the resulting live heap, radio latency or display timing.

| Implemented change | Measured or source-established saving | Evidence boundary |
| --- | --- | --- |
| Share display/game pixel workspace; retain encoded custom clips only during play/pause | **22,455 B less BSS**: display 12,219 B + arcade 10,236 B; text grows 536 B | Candidate-only; 16 host regression tests captured 19,154 SPI frames across default/candidate mode and failure transitions; physical timing remains open |
| Create the release worker only for checks/downloads | **10,240 B idle stack** plus TCB, from the task allocation | Candidate-only; six-hour scheduling, explicit requests and operation locking remain; actual free-heap change unmeasured |
| Copy only the relay prefix actually needed | **2,056 B less BSS** | Preserves the existing connection and queue semantics; default-off build remains unchanged |
| Share WiZ request/response storage and receive directly into the bounded caller buffer | **4,100 B less retained driver + adapter storage**; the earlier runtime aggregate fell by 4,104 B including alignment, before RC16's separate 504 B helper addition | C3 DWARF and host malformed/oversized-response tests; UDP hardware tests remain open |
| Reuse the outbound relay connection | Avoids a second permanent WebSocket/TLS stack/session | Bounded lighting messages share the existing relay; concurrent hardware behavior unverified |

These are different kinds of savings. Static savings are already included in
the final link table; do not add them to it again. Runtime context savings and an
idle stack release are separate from linker free bytes. In particular, adding
these numbers to a historical HTTPS status snapshot is not an idle-heap
measurement or proof that admission succeeds.

The following remain proposals, not implemented savings:

| Candidate | Size evidence | Required constraint |
| --- | --- | --- |
| Free SRP state after successful establishment | About **3 KB/session**, estimated | Separate vendored change and cleanup/crypto tests above; not yet implemented |
| Further reduce relay queue storage | Existing full-size ingress queue remains bounded | An ownership-preserving pool would need burst and ownership regression tests; do not silently shrink the queue |
| Static Wi-Fi RX buffers 10→6 | About **6,400 B** initialization-heap saving, not measured here | RX BA window is 6; reduced burst tolerance/throughput must pass campus, OTA and BLE tests |
| Reduce diagnostic history | 64 × 144 B currently; half saves 4,608 B | Explicit retention tradeoff, not free behavior-preserving optimization |
| Smaller authority ledger | Current authority size includes 32 entries | Only with rate/TTL/idempotency semantics re-proven; no unvalidated eviction of live commands |

Dynamic Wi-Fi RX/TX caps currently 32/32 can bound burst peaks but do not guarantee
an idle saving. Static RX buffers are allocated at Wi-Fi initialization, not in
BSS. `ESP_WIFI_EXTRA_IRAM_OPT` is already off. The implemented display/game
workspace sharing relies on explicit ownership and frame regression tests;
further buffer or stack reductions require equivalent evidence and measured
watermarks. The display task is 4,096 B at priority 1, arcade 4,096 B at priority 2,
health 4,096 B at priority 4, relay 8,192 B plus WebSocket task 6,144 B, and HTTPS
stack 12,288 B. [Wi-Fi buffer tradeoffs](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-guides/wifi-driver/wifi-performance-and-power-save.html).

## CPU, radio and admission gates

ESP32-C3 has one 2.4 GHz radio. Espressif marks STA+BLE supported/stable but
SoftAP clients connecting/connected together with BLE as supported with unstable
performance. The proposed campus STA + private bulb AP + BLE configuration
therefore requires direct qualification. Lower linked RAM does not establish
latency, throughput or coexistence. The installed baseline AP caps clients at 4;
the integrated lighting candidate raises this to 8 for bulbs, players and
maintenance. That capacity and its additional runtime budget are untested.
[C3 coexistence matrix](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-guides/coexist.html).

The default NimBLE host runs at `configMAX_PRIORITIES-4`, above display priority 1.
BLE callbacks must only validate/copy bounded fragments and notify an existing
worker. SRP, JSON, NVS and UDP work must stay outside that host callback. A
low-priority application worker and bounded queues help, but single-core crypto,
flash-cache stalls and radio preemption still need measurements.

The following are **provisional preproduction gates**, not validated tuning
values or permission to flash:

| Gate | Required result |
| --- | --- |
| Signed image reserve | ≤1,835,008 B (`0x1C0000`), including signing padding; leaves 128 KiB in each existing slot. RC16 passes this image-size gate. |
| Worker creation | Actual implementation requires ≥109,568 B free internal 8-bit heap (96 KiB allowance +10 KiB worker +1 KiB task margin), largest block ≥24,576 B: max(24,576, 22,120 +1,024). RC16 has not been measured on hardware at this gate. |
| Runtime activation | After worker creation, ≥98,304 B free and ≥24,576 B largest block before allocating the retained context. These are provisional safeguards, not measured operating requirements. |
| Each startup phase | Preserve ≥32,768 B free internal heap after allocation/init; unwind lighting on failure, keep display and existing management alive. |
| Inactive runtime recovery | If configuration remains valid and startup was refused for resources, retry through the existing relay loop after 30 seconds, doubling up to five minutes. Do not retry during OTA/quiescence, without configuration, on storage corruption, or while another worker exists. |
| SRP admission | Initially require ≥65,536 B free and ≥24,576 B largest block; only one handshake in progress; prohibit overlapping peak TLS/release/OTA operations through a shared reservation policy. |
| Permanent health floor | Preserve current **free heap >24,000 B** rule. Do not lower it to make the feature appear healthy. |
| Session/traffic bounds | At most 2 BLE links and 3 secure contexts, bounded 1024 B ciphertext packets, finite queues, deadlines and cleanup on disconnect. |
| Revised numerical thresholds | Use measured worst-case incremental peak +32 KiB floor +8 KiB margin; separately cover largest allocation/fragmentation. Change defaults only with recorded measurements. |
| Display timing | Use the stricter architecture gate: fresh matched baseline, ≤5% increase in p99 frame/tick duration, no new >250 ms gap and no accumulating simulation backlog. Record late-frame rate as well. |
| Stability | At least 60 minutes with 5 real bulbs, 2 BLE clients, relay traffic and ordinary management access; no unexplained reset, progressive heap loss, corrupted settings or management loss. |

Instrument before/after BLE initialization, each connection, both SRP steps,
three established contexts, malformed-message storms, maximum UDP responses,
policy commits, campus disconnect/rejoin, TLS reconnect, release check and signed
OTA. Capture minimum heap, largest block, allocation failures, per-task stack
watermarks, frame-time distributions and command latency. Exercise flash
erase/write while BLE is connected; source support for auto-suspend is not that
test. Repeat disconnect/reconnect and allocation-failure cleanup to prove memory
returns. Do not weaken watchdogs or existing healthy-trial confirmation.

The final runtime coordinator and root transport integration have linked. This
build audit does not validate their execution, AP capacity, optional Internet
forwarding to bulbs, real commissioning or combined hardware behavior. Historical
RC15 free heap plus the static saving does not establish that the activation
gate can pass. At the build checkpoint, production remained on RC15. RC16 is a
signed candidate for controlled qualification, not evidence that these gates
have passed or that routine unattended lighting deployment is ready.

Status diagnostics distinguish the failed startup/admission stage, its reason,
captured free heap/largest block, required thresholds, and bounded retry timing.
The pure queue and recovery suites passed ASan/UBSan checks, including backoff,
eligibility, single-attempt consumption and overflow. These do not exercise
FreeRTOS allocation failure or startup recovery on a physical C3. RC16 also adds
boot-local quarantine after abnormal resets and bounded sampling of its own
worker's minimum unused stack once per second; neither is a measured hardware
recovery/stack result yet.

## Evidence files and reproduction

Run from the repository root using the pinned Python environment:

```sh
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/firmware/probe/measure.py
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/firmware/probe/combined.py --with-controller-flash
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/firmware/probe/combined.py --with-transport --with-gateway --only wifi-bt-suspend
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/tools/build_candidate.py
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/tools/build_candidate.py --default-off
```

The exact RC16 hardware build used the clean source commit recorded above:

```sh
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/tools/build_candidate.py --hardware-candidate --version 0.3.0-rc.16
```

Use that source checkpoint to reproduce the recorded version. Changed sources
or build inputs require a new explicit prerelease; the builder refuses to
reassign an existing hardware version to different inputs. Hardware mode only
builds and verifies local artifacts; it does not install them.

Local evidence is intentionally ignored: `probe-summary.json`, `baseline-size.json`,
stage maps/size reports, `c3-type-sizes.json`, `c3-gateway-type-sizes.json`, and
combined per-profile summaries/logs/configs under `.local/lights-resource-probe/`.
The historical disposable-key candidate adds `.local/lights-candidate/summary.json`,
`type-sizes.json`, `signature-check.log`, `size.json`, `archives.json` and the
retained build outputs. It passes build, test-key signature, reserved image size,
SDK logging caps and production-artifact preservation checks only.
The independent `.local/lights-default-off` build passes its disabled-Bluetooth,
signature, size and production-preservation checks with a different disposable
key. The later local aggregate checkpoint passed **26 groups with zero skips**,
including 14 builder-safety tests, five runtime/local-time sanitizer suites,
39 web tests, 16 web-security tests, 45 relay tests and the native checks. Its
root regression passed 93 tests. The final quarantine/stack additions then
received six targeted suites and a 96-test root regression, recorded in the
[validation report](validation-2026-11-16.md); these are separate checkpoints,
not a claim that the earlier aggregate covered later edits. Local checks do
not establish a hosted CI pass.

The RC16 build adds `.local/lights-hardware/0.3.0-rc.16/summary.json`,
`type-sizes.json`, `signature-check.log`, `baseline-signature-check.log`,
`signature-block-audit.json`, `size.json`, `archives.json` and retained build
outputs. It passed clean-source, pinned-SDK, production-key signature, image-size,
resolved-profile, sensitive-log-cap and artifact/key-preservation gates. Its
provenance explicitly records `test_key_only: false`,
`hardware_qualification_only: true`, `flashed: false` and
`runtime_qualified: false`. These flags describe the builder's completed action,
not subsequent device state. Installation and physical qualification remain
separate records in the [validation report](validation-2026-11-16.md).
Do not publish the local directory or treat a test-key binary as a release.
This report contains no device credentials, Wi-Fi passwords or production keys.
