# 203 Lights

Native macOS lighting controls and a bounded WiZ gateway candidate for the
existing ESP32-C3 display. **This is a development implementation, not a
qualified lighting deployment.** No physical bulb has been commissioned or
controlled by these checks. The installed display keeps its existing animation
and games; lighting has no screen content.

## Components and trust

| Component | Responsibility |
| --- | --- |
| [macOS menulet](macos/README.md) | Native SwiftUI controls, capability-aware selections, pending/reported state, owner settings and Keychain integration |
| [Browser client](../sites/lights/) | Optional Internet controls using the same authenticated gateway protocol |
| [Security2](security/README.md) | SRP authentication, AES-GCM records and binding to an independently trusted gateway identity |
| [BLE transport](firmware/ble/README.md) / [relay lane](../relay/LIGHTS_PROTOCOL.md) | Bounded byte transport; transport acceptance never grants lighting authority |
| [Gateway service](firmware/gateway/README.md) / [authority](firmware/core/README.md) | Owner/guest grants, revocation, password rotation, freezes, owner precedence, deadlines, persistence and command reconciliation |
| [WiZ driver](firmware/wiz/README.md) | Allowlisted local UDP to enrolled bulbs associated with the ESP's local AP |

Internet clients use the ESP's existing outbound relay connection; nearby Macs
can use authenticated BLE. A route change requires fresh authentication, and
uncertain writes are reconciled rather than replaying old ciphertext. Lighting clients never receive the WiZ signing key. The existing arcade AP
password is guest-known; it is not a secret isolation boundary for bulbs. With
the owner's chosen “Allow all controls” setting, direct local control can bypass
our app/site freezes. The owner access key,
guest password, WiZ home key and firmware signing key are separate credentials.

The chosen topology keeps the installed C3 as the gateway and the spare as a
maintenance hub. Campus peer isolation means bulbs need the local ESP AP for
local UDP. **The ESP's campus Internet connection does not itself give AP bulbs
Internet access:** forwarding is not implemented. Actual commissioning, bulb
clock bootstrap for timestamped commands, signing-key retention after reset and
rejection of unsigned requests still need physical verification. See the
[WiZ capability research](../docs/lights/wiz-research.md) before promising app-free
onboarding or full WiZ accessory/settings parity.

## Build and check locally

Use the repository's existing pinned ESP-IDF v6.1 setup, Python environment and
resolved managed components. Host checks require a C compiler, CMake, Make,
Node/npm and Python. Swift checks require macOS and Apple's Swift toolchain.
SwiftPM may fetch its exact pinned dependency on the first build; the checks do
not connect to a bulb, Bluetooth peripheral or gateway.

```sh
bash lights/scripts/check-all.sh
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/tools/build_candidate.py
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/tools/build_candidate.py --default-off
bash lights/macos/scripts/build-app.sh release
```

The central runner covers native C policy/protocol/storage/queue checks with
sanitizers, pinned fixtures, relay tests, browser security/model tests, web
packaging and macOS Swift checks. Logs are in `.local/lights-checks/logs`.
Non-macOS Swift checks, an absent optional upstream fixture checkout, and an
absent optional native wire-test Python environment are explicitly reported as
skipped. `bash lights/security/scripts/bootstrap-test-env.sh` prepares the pinned
local Python peer used by the native adapter's encrypted fixture tests; it does
not create device credentials. `--skip-swift` and `--skip-oracle` intentionally
reduce coverage; a passing partial run is not full qualification. Browser UI,
live transports and hardware are separate tests.

The repository CI now includes portable lighting checks and the isolated
candidate in its Linux firmware job, plus a separate macOS native/security/wire
job. The native job explicitly selects Xcode 26.6 on the macOS 26 runner rather
than depending on the runner's changing default. The selected toolchain is listed
in [GitHub's runner image inventory](https://github.com/actions/runner-images/blob/main/images/macos/macos-26-arm64-Readme.md).
Hosted execution of these new jobs must be checked after the source is pushed;
local passes are not a hosted-CI result. Production signing credentials are never
provided to CI or included in its artifact paths.

The candidate builder enables `ENTRANCE_LIGHTS=ON` only for its isolated
`.local/lights-candidate/build`, preserving the production configuration/build.
It starts from the current production configuration, applies the measured
BLE/Wi-Fi/flash profile and bounded 20-socket lighting budget, disables registry resolution and signs with a newly
generated local **disposable test key**. It verifies that signature, records
size/provenance, checks SDK debug-log caps and enforces a signed-image reserve.
It never flashes or publishes. Its image is not trusted by the production
device. Do not run it concurrently with another manual build in that directory.
`--dry-run` prints paths and flags without changing files.
`--default-off` separately checks the ordinary configuration in
`.local/lights-default-off` with its own disposable key, preserving both the
enabled candidate and production artifacts.
The socket budget covers concurrent management, relay, WiZ and local time/DNS
services with release headroom; the default-off profile keeps its existing 16.

For an explicitly planned hardware qualification, the builder has a separate
production-key mode. It requires a clean committed worktree, the existing
owner-only `.local/keys/firmware.pem` matching tracked `keys/release-public.pem`,
and an explicit prerelease newer than the retained signed baseline. For example,
after committing reviewed sources:

```sh
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/tools/build_candidate.py --hardware-candidate --version 0.3.0-rc.16
```

This writes only `.local/lights-hardware/<version>`, verifies the baseline and
candidate signatures, and records the clean commit, tree, input/dependency
digests and signing public-key fingerprint. A tag is optional for this local
qualification; stable publication remains a separate release workflow. It never
generates, replaces or copies the production private key. A previously built
hardware version cannot be reassigned to different source inputs. This mode
does **not** flash the board, publish a release, or declare the hardware gates
passed. The default build and CI still use disposable keys. `--dry-run` does not
check source cleanliness or key identity and cannot authorize installation.

The ordinary firmware build defaults to `ENTRANCE_LIGHTS=OFF`. The enabled
candidate remains dormant until trusted owner provisioning; it must not expose
a public first-claim operation. The separate [access tool](tools/access.py)
prepares a private owner recovery bundle and provisions only through the
existing authenticated, certificate-pinned management API. Do not distribute
that bundle. Provisioning, flashing and bulb resets are not part of host checks.

After a panic, watchdog, brownout or other abnormal reset, the candidate leaves
lighting quarantined while display and management remain available. Check
`python3 lights/tools/access.py status --host <local-hub-endpoint>` first, then
explicitly use `python3 lights/tools/access.py retry --host <local-hub-endpoint>`
to release that boot's quarantine through the pinned management admin API.
Retry closes its TLS connection immediately; startup may wait for the existing
memory-recovery backoff. It changes no credential or light record and does not
prove activation succeeded. The [runtime lifecycle](firmware/runtime/README.md#abnormal-reset-containment)
documents the exact reset policy and limits. This recovery path needs local
management reachability; the inactive lighting relay cannot perform it.

The native build creates `macos/build/203 Lights.app` without installing or
launching it. Normal launch is disconnected; `--preview` explicitly shows sample
lights. Local ad-hoc signing is not Developer ID signing or notarization, so this
bundle is not yet a general distribution release.

## Release gates still open

- Demonstrate real commissioning, supported bulb features, signed-command time
  bootstrap and the actual AP access boundary without changing the Mac's network.
- Validate the same C3 with five bulbs, concurrent BLE/Internet clients,
  authentication storms, campus reconnects and NVS/OTA writes. Measure heap,
  largest allocation, stack watermarks and display/game timing; keep the current
  health floor and fail closed when resources are insufficient.
- Complete interruption/recovery checks, OTA regression and the combined
  60-minute soak. A compiled image or simulated UDP success is not this evidence.
- Finish browser/physical-client accessibility and live transport tests, then
  distribution signing/notarization and credential/recovery handoff.

The [resource audit](../docs/lights/resource-budget.md) records isolated image/RAM
measurements; the [architecture](../docs/lights/gateway-architecture.md) defines
acceptance gates and the [native UX document](../docs/lights/native-ux.md) records
interaction decisions. Advanced WiZ schedules/rhythms, power-on settings,
accessories and vendor firmware updates are not claimed as implemented parity.
