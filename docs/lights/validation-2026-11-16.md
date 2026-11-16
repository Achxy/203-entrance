# 203 Lights development validation — 16 November 2026

The web interface and relay readiness endpoint are published. The native app and
ESP32 lighting implementation remain development candidates. **Signed RC16 is
installed and boot-confirmed on the production display, but lighting provisioning
was safely refused for insufficient memory. No lighting credentials were
committed and no physical bulb has been paired, reset or controlled by this
implementation.**

## Live services

| Item | Verified result |
| --- | --- |
| [Lighting website](https://iot.203.at.achyuth.dev) | Custom hostname active, valid HTTPS, HTTP 200; real browser rendered the disconnected interface |
| DNS | CNAME and both requested TXT records saved in deSEC and checked against the authoritative nameservers |
| Site publication | Version 3, `appgprj_6afa17238cd48191b9a4b2e5fa536b8a~appgver_0a06cc6cc5348191b0300ef4c8f60eea`, source commit `2b2d183cae0c51d25c59ce1dabf1b1f1c638be8b`, successful deployment `appgdep_6afa341516908191a77d82c982e2fa26` |
| Relay publication | Cloudflare version `58aaf093-2bd1-4cb4-950e-ec51a5818586`; `LIGHTS_ENABLED=false` |
| Readiness | Allowed-origin `/v1/lights/info` returns the public pinned identity and `online:false`; unrelated origin returns 403; disabled lighting socket returns 404 |
| Existing display relay | `/v1/status` still returned `online:true` after relay deployment |

The lighting offline indicator does not mean the existing display is offline.
It reflects the unprovisioned lighting service. Enabling the relay
flag alone cannot provide authenticated lighting control.

The owner confirmed that WiZ currently allows all local controls and explicitly
asked to leave it that way. Both clients distinguish signed gateway writes from
bulb-side rejection of other clients. Guest pauses/revocations apply to this
app/site; another client on the bulb Wi-Fi may bypass them. Importing a home key
does not change that setting. Version 2's owner notices were checked in the live
browser using explicitly labeled, network-free preview data. Version 3 preserves
those interface assets and serves them from a Worker with explicit security
headers. Live GET/HEAD, 404 and 405 responses all returned the expected CSP,
frame-embedding restriction, MIME protection, referrer/permissions policy,
origin isolation and no-store policy. The browser rendered the updated site.
The private header evidence is `.local/lights-ui/site-v3-header-evidence.json`.

## Host and build evidence

- The full existing repository regression suite passed **98 tests**, including
  OTA, image/manifest policy, settings, USB hub, display and game regressions.
- The memory-sharing display harness passed **19,154 complete SPI-frame
  captures**, including every built-in animation frame, orientations, captions,
  entry to and return from games, custom media transitions and failure cases.
  These are host captures, not a physical-panel timing measurement.
- Native UI/model/transport checks passed **100 assertions**. The separate
  encrypted adapter suite passed **52 assertions** using actual Swift SRP,
  signed gateway binding and AES against a scripted local Python peer.
- Security/verifier checks passed **17 groups, 351 assertions**. Browser
  cryptographic fixtures passed 33 assertions in real Chrome, including short
  and leading-zero SRP cases. Chrome desktop/mobile layout, disabled controls,
  overflow and failure presentation were inspected. This is not iPhone/Safari
  or ESP32 interoperability proof.
- The native live-diagnostics command initially passed **53 offline assertions** for
  private-file handling, fixed gateway identity, bounded execution and sanitized
  evidence. Its default invocation is inert; the actual gateway remains untested
  until an explicit live run succeeds.
- Both native arm64 and universal arm64/x86_64 development packages passed
  architecture, macOS 14 target, strict signature and extracted ZIP hash checks.
  They include complete third-party notices, manifests and file hashes. These
  are ad-hoc signatures; neither package is notarized, and Intel runtime behavior
  remains unverified. Fourteen
  NSHostingView captures cover light/dark and ready, disconnected, frozen,
  partial and long-name states. Native CUA attachment timed out, including a fresh full-path retry; interactive
  menu behavior, real Keychain access and VoiceOver remain
  unverified. See [native validation](../../lights/macos/VALIDATION.md).
- The isolated default-off firmware regression build retained the RC15 signed
  size of **1,314,816 bytes** and static RAM of **164,362 bytes**. The production
  build/configuration were protected and verified unchanged.
- The isolated lighting candidate fits the two-slot layout and is signed with
  a **disposable test key**. The final image hash, static RAM and runtime object
  sizes are recorded by the [candidate builder](../../lights/tools/build_candidate.py)
  in ignored `.local/lights-candidate/summary.json` and summarized in the
  [resource audit](resource-budget.md). That image is not trusted by production.

The aggregate checkpoint passed **26 groups with zero skips**, including
39 web tests, 16 browser cryptographic tests, 45 relay tests, the native
encrypted-wire suite, builder safety checks and the runtime/local-time
sanitizer suites. The bounded startup-recovery and local-time changes also
passed independent source review. The final default-off rebuild passed with Bluetooth disabled and the original
16-socket budget, retaining 1,314,816 signed bytes and 164,362 bytes of static
RAM. The production-key candidate measurements are recorded separately when
complete. Configured but inactive runtimes retry transient
resource refusals after 30/60/120/240/300 seconds without a new scheduler task.
Uncommitted first-time provisioning still requires an explicit retry.

A later safety review added boot-local lighting quarantine after panic, watchdog,
brownout, unknown and other abnormal resets. Ordinary power-on, software reset
and deep-sleep wake retain normal startup. A quarantined boot keeps the display
and management available; only one authenticated local-management retry can
release lighting for that boot, with the same allocation guards. No credentials
are erased and no persistent boot ledger is written. Targeted sanitizer/C3 and
management-tool checks passed; the subsequent full repository regression passed
96 tests. Actual crash-containment behavior still needs hardware validation.
The runtime also exposes a bounded, once-per-second measurement of its own task
stack's minimum unused bytes; no physical stack result is claimed yet.

## Production observations and private artifacts

An authenticated read through the spare USB-to-Wi-Fi hub found the installed
display still on **0.3.0-rc.15**, with healthy settings, an available rollback
target and animation slot 2 playing. Its captured uptime was about 113 minutes;
this is **not** evidence of a continuously monitored 60-minute lighting soak.
At that authenticated sample, free heap was 55,872 bytes and the largest block
was 36,864 bytes. Active TLS and current radio state affect these numbers, so
they are not substituted for candidate peak measurements.

The WiZ development export and generated owner recovery bundle are separate,
ignored private files under `.local/lights/`, with private filesystem modes.
No home key, owner key or guest password is bundled in the app/site. A scan of
607 tracked/unignored source files found zero matches for those actual private
credentials. The later first provisioning attempt and its safe refusal are
recorded below.

The Mac's Wi-Fi, routes and network configuration were not changed. The spare
remained a maintenance hub; it was not converted into the lighting gateway.
After the owner identified the bulbs' current hotspot, a spare-only Wi-Fi scan
found it at approximately −55 dBm. A fresh 4,194,304-byte hub flash backup was
read and hash-verified before developing the bounded read-only WiZ diagnostics.
Its private recovery path is `.local/backups/20261115T213954Z/`.

The spare was then updated to **hub 1.1.0**, with verified write hashes. Its
bounded read-only diagnostic discovered one of the four exported bulbs on the
owner's existing hotspot and received all five requested replies:
`getSystemConfig`, `getModelConfig`, `getUserConfig`, `getPilot`, and `getTime`.
The responding module is **ESP25_SHRGB_01**, firmware **1.38.0**; its model
reports a 10% minimum dimming level and a 2200–6500 K range. Its clock was about
two seconds behind the host sample. These unsigned replies are correlated
diagnostic observations, not cryptographic bulb authentication, signed-write
acceptance, or verified-only enforcement. No bulb setting changed.

The identity-free model response is retained as a parser regression fixture.
The full record, containing home/device identifiers, remains private at
`.local/lights/diagnostics/wiz-read.json`. The existing C parser accepted the
actual response unchanged; 10,838 C assertions and 52 upstream fixtures passed.
`hasGradient` does not by itself enable unsupported gradient/zone controls.

After those reads, the spare rejoined only the production hotspot. Two pinned,
authenticated HTTPS reads verified RC15, healthy settings, and advancing slot-2
animation: rendered frames increased from 112,061 to 112,547 and loops from
2,924 to 2,937. The public display relay independently returned `online:true`.
The private before/after record is `.local/lights/production-post-hub.json`.

### Baseline monitor interruption

The fresh RC15 host-monitored soak **failed**, with 148 accepted samples over
2,399 seconds, because the monitor exceeded its 45-second telemetry-gap limit
while renewing its management session. The retained failed report is
`.local/lights/rc15-baseline-soak.json`; it is not a completed 60-minute gate.
A separate diagnostic measured password verification at 33.295 seconds and a
status request at 1.284 seconds. The CLI had added its normal 15-second sleep
before renewal. It now samples immediately before and after renewal, without
relaxing authentication or the gap limit. Two command-level clock/device tests
verify that a 33-second renewal completes a simulated hour, while a 46-second
renewal still fails and preserves its evidence.

The board's independent 64-entry, one-minute health history covered 3,782
seconds on the same boot, with zero unhealthy observations and settings
generation 8 throughout. This is separate, lower-frequency evidence; it does
not establish uninterrupted animation or replace the failed host gate. Its
heap samples span periods both with and without an open management connection,
so their first/last medians are not a controlled heap-leak measurement. The
private diagnostic is `.local/lights/rc15-monitor-gap-diagnostic.json`.

### RC16 installed; first provisioning refused safely

The isolated hardware candidate was built from clean commit
`54bdb752c6e5c6d805647809461a839241fccfc7`, using the existing production key.
Its signed image is 1,708,032 bytes, SHA-256
`5d3ee49dc711360821b9dd74cd1bf160146da2bfd773cb7e9287b3a9e745ee8c`,
with 258,048 bytes of slot headroom. The independent signature-sector audit
found exactly one matching RSA-3072 signature block and erased unused space.
Static RAM is 129,269 bytes; the lazily allocated runtime is 22,120 bytes.
See the [resource audit](resource-budget.md) for link-time versus runtime limits.

The upload through the spare hub completed and the candidate passed its normal
healthy trial/reconnection confirmation. RC15 remained the available rollback
target. No Mac Wi-Fi or network setting changed. The private installation log
is `.local/lights/rc16-install.log`; build provenance and artifacts are under
`.local/lights-hardware/0.3.0-rc.16/`. Build provenance describes the artifact
at creation, so its original `flashed: false` field is not a current device-status
record and has not been rewritten.

After initial transient post-reboot connection refusals, authenticated reads
verified the expected version, healthy settings and slot-2 replay. Rendered
frames advanced by 2,048 and loop count by 55 over 163,309 ms. Captions retained
their 1,350 ms period and landscape dimensions remained 160 × 128. Onboard
minute samples during the quiet interval reported 110,128 and 110,160 bytes
free, with a 94,208-byte largest block. These are point measurements, not a
completed soak or loaded-lighting performance qualification. The private
record is `.local/lights/rc16-first-boot.json`.

The first zero-bulb provisioning request returned HTTP 409 / `ESP_ERR_NO_MEM`.
Its worker-admission snapshot measured **103,908 bytes free**, below the
unchanged **109,568-byte** requirement; the 94,208-byte largest block exceeded
its 24,576-byte requirement. The worker was never started, configuration remained
absent, provisioning was no longer pending, and storage remained healthy.
No automatic retry was armed for the uncommitted request. A subsequent read
still showed advancing animation (5,215 frames / 138 loops at uptime 419,597 ms)
and a lifetime minimum heap of 77,968 bytes. Evidence is in
`.local/lights/rc16-provision-attempt.log` and
`.local/lights/rc16-provision-refusal.json`.

This result motivates bounded deferred provisioning after the initiating TLS
connection closes and further measured memory savings. It does not justify
lowering the allocation or health guards, and activation alone would not prove
that both SRP handshake steps have sufficient memory.

### Follow-up candidate checks

The deferred provisioning implementation accepts one explicit request, closes
the initiating TLS connection, and admits the worker within a fixed 30-second
deadline. The existing health loop expires and wipes an unclaimed request even
when relay DNS is stalled. The CLI correlates boot and generation, observes the
result with short connections, and never silently repeats an uncertain write.
All existing resource thresholds remain unchanged. Independent review covered
the deadline/commit boundary and private-copy lifetime.

The lighting build now uses five owned small relay payload slots and guarded
exact-size allocations for larger messages. Independent review verified release
on handling, refusal and stopped-producer cleanup against the pinned WebSocket
source. The initial gross idle estimate was
7,682 bytes saved, while the all-large burst can retain 2,563 bytes more than the
previous fixed payload storage. See [ownership and bounds](../../components/entrance/relay_ingress.md).

The complete repository regression passed **111 tests**. The aggregate lighting
suite passed **26 groups with zero skips**, including the updated runtime,
native diagnostic probe and encrypted-wire checks. The relay ownership tests
include 1,000 reconnect/drain cycles and 20,000 concurrent submissions under
ASan/UBSan. These are host results; the follow-up firmware has not yet been
installed or qualified. Private logs are `.local/lights/rc17-regression-tests.log`
and `.local/lights/rc17-host-checks.log`. A scan of 619 tracked/unignored files
found no matches for the actual private lighting credentials and no files over
5 MiB.

The frozen diagnostic app passed **111 offline assertions** and strict local
signature checks. A fresh launch at 09:14 UTC verified that macOS Bluetooth
authorization is already allowed. The evidence is
`.local/lights/ble-permission-20261116-retry.json`. This permission check did not
scan for or authenticate a gateway and does not prove live BLE operation.

The resumed spare-radio path suffered TCP/TLS failures, then successfully read
the display's local HTTP arcade status at about 19 minutes uptime. That public
response alone does not identify a reset cause or establish firmware health.
The USB hub subsequently re-enumerated when the owner attached another adapter;
the spare was reidentified by its expected hardware MAC before reconnecting.
The Mac's Wi-Fi, default route and DNS were not changed.

### RC17 built and audited; two transfers did not activate

Clean source `7032ef67296c9111728676e375c2f1284a16209e` produced signed RC17,
SHA-256 `d5cfb3e9248f22ab1ede6dba554ff42b41a76b4790aafb896ec0cbd4bfdbcf10`.
Its 1,708,032-byte size leaves 258,048 bytes of slot headroom. An independent
artifact audit verified RSA-3072/PSS, the embedded public key and Montgomery
values, signature-block CRC, the hash of all pre-signature bytes, and the
2,880-byte erased signature-sector remainder. Target, version, layout and schema
match; fault injection is disabled. Software-only signed OTA and rollback remain
enabled, with hardware secure boot, encryption and anti-rollback disabled.
The partition binary matches the retained baseline.

Linked static RAM is **131,933 bytes**, 2,664 bytes more than RC16, entirely BSS.
Removing 10,248 bytes of retained dynamic relay payload storage gives a nominal
**7,584-byte idle reduction** before allocator overhead. The runtime object
remains 22,120 bytes. This is target build accounting, not a live RC17 heap or
authentication measurement. Protected production configuration, RC15 recovery
artifacts, the RC16 image and the retained earlier default-off image are unchanged.

[All five CI jobs passed](https://github.com/Achxy/203-entrance/actions/runs/43840647310):
Linux firmware and lighting checks, native macOS app/wire checks and universal
packaging, console, arcade site, and relay. The signed local candidate remains a
qualification artifact; it was not published as a firmware release.

Two bounded hub uploads failed before activation. The first reached at least
655,360 bytes before the board reported a transfer timeout; the second exhausted
the uploader's three chunk attempts. Both logs are retained separately as
`.local/lights/rc17-install.log` and `.local/lights/rc17-install-attempt-2.log`.
No timeout or resource threshold was weakened and no third upload was started.

A final pinned, authenticated read verified **RC16**, a non-trial boot at
2,201,607 ms uptime, reset reason 1 (power-on), healthy settings and healthy
services. Animation slot 2 remained in replay mode, with 25,038 rendered frames,
663 replay loops and the existing 1,350 ms caption period. The failed upload was
aborted; its phase was `failed`, with no active trial. **The inactive application
slot was partially overwritten, so `rollback_available` is now false.** The
running RC16 image is intact; local signed recovery artifacts are retained.
This is the expected distinction between preserving the running image during an
interrupted transfer and preserving an older image in the slot being replaced.
Evidence is `.local/lights/rc17-after-interrupted-transfers.json`.

A private phone-update ZIP was prepared and checked for archive integrity,
exact image/certificate hashes and exclusion of the administrator password.
It contains the signed image, public device certificate, hashes and instructions;
credentials remain in a separate private local file. Phone/browser certificate
handling and the actual phone upload remain untested. Lighting provisioning,
live BLE authentication, bulb writes and the combined 60-minute gate remain pending.

The owner's additional USB adapter enumerated as TP-Link/Realtek `2357:0120`
without a usable network interface. Static inspection of the supplied
`Archer T2U Plus _V1_190111_Mac.zip` found only Intel kernel-driver executables
in a package labelled macOS 10.9–10.14; this Mac runs macOS 26.5.2 on arm64.
The separately inspected experimental driver has no ready route-free TCP bridge
and lists Tahoe as untested. No driver was installed or executed and no Mac
network/security setting was changed.

## Remaining acceptance work

1. Measure the enabled firmware's activation, SRP, BLE, TLS reconnect, flash
   writes and five-bulb workload on hardware without reducing the current
   health floor. Verify bounded retry and the refusal diagnostics.
2. For the identified ESP25/1.38 bulb, establish signed-control behavior,
   verified-only enforcement, readback and cold-start time recovery. Preserve its
   current home before attempting independent commissioning.
3. Finish the standalone lifecycle: pairing, key establishment, removal,
   re-enrollment and recovery. Official WiZ setup/export remains a temporary
   development bootstrap. Current research is in
   [the standalone commissioning note](wiz-standalone-commissioning-research.md).
4. Qualify live browser/native Internet and BLE paths, policy races and failure
   recovery with the display running, followed by the combined 60-minute soak
   and relevant OTA/power-interruption tests.
5. Finish native interactive accessibility and real signed-app Keychain tests,
   then Developer ID signing, notarization and Gatekeeper testing for public
   distribution. No distribution identity is currently available.

Core local controls are implemented: power, brightness, white temperature,
RGB, supported scenes and effect speed, plus our owner/guest policy. Advanced
WiZ schedules/rhythms, accessories, native power-on configuration and vendor
firmware administration are not claimed as complete parity. The
[capability research](wiz-research.md) tracks those separate requirements.

## External adapter investigation

The subsequent [USB Wi-Fi adapter investigation](usb-adapter-research-2026-11-16.md)
produced a bounded, route-free transport prototype and passing host tests, but
the adapter's radio has not completed startup. No RC17 installation or lighting
qualification is claimed from that work. A fresh pinned status read through the
existing spare hub still found healthy RC16, settings intact and animation slot
2 active. The Mac's built-in Wi-Fi and default Internet route were preserved.
