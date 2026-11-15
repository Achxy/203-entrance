# 203 Lights development validation — 16 November 2026

The web interface and relay readiness endpoint are published. The native app and
ESP32 lighting implementation are development candidates. **Lighting has not
been installed or provisioned on the production display, and no physical bulb
has been paired, reset or controlled by this implementation.**

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
It reflects the uninstalled/unprovisioned lighting service. Enabling the relay
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

- The full existing repository regression suite passed **96 tests**, including
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
- The native live-diagnostics command passed **53 offline assertions** for
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
  menu behavior, real Keychain access, permission dialogs and VoiceOver remain
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
credentials. No production lighting provisioning request was sent.

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
