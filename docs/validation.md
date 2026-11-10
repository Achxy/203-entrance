# Validation record

Status: **qualification in progress; stable acceptance has not been claimed**.

The `0.1.0` prerelease was superseded during qualification. An additional storage-failure test found that the Wi-Fi driver's NVS dependency prevented maintenance startup when the settings partition was unavailable. The corrected initialization explicitly disables driver-owned NVS. Actual certificate-pinned HTTPS maintenance login/status now succeeds with the settings NVS deliberately unavailable, without erasing saved settings. Fresh normal-operation stability qualification follows this correction; eight earlier successful alternating cycles are retained as historical evidence and will not be counted toward the corrected firmware's 20-cycle gate.

## Hardware and preservation

- Attached Espressif ESP32-C3 QFN32 revision 0.4, 4 MB embedded XMC flash, 40 MHz crystal, USB Serial/JTAG.
- Original full flash saved before writing; 4,194,304 bytes, SHA-256 `a0f6eb5c4d053e689d5dcd3723e0908319e4d5fcf067344cbfe001b2c639e1db`.
- A second full pre-install backup completed with metadata under private `.local/backups/`.
- Signed initial firmware and unique identity were installed over USB. Bootloader, partition geometry, native signature and flash-write hashes were checked.
- Hardware Secure Boot and flash encryption were disabled during inspection. No eFuse write command has been used. A final security-state read remains part of acceptance.

## Automated evidence

- 37 host test methods pass, with additional subcases: actual C SemVer/compatibility/CRC/hex logic, actual C request/manifest parser, authentication/throttling/CSRF policy, copy-on-write settings selection and schema compatibility, RSA manifest verification and wrong-key/tampered/truncated cases, and bounded upload acknowledgement recovery.
- The production `ota.c` runs in a native harness with simulated flash, time, mutexes and native-verification outcomes. It covers concurrent starts, bounds/offsets, transfer timeout, image incompatibility/downgrade, digest/signature/write/journal/activation failure, continuous health, scoped ticket, deadline, unavailable rollback, and reset windows. Hash and flash doubles do not establish real flash/RSA/power-loss behavior.
- Signed ESP-IDF builds pass native `espsecure` verification and slot limits. The tested rc.2 image is 1,118,208 bytes, leaving 847,872 bytes in each slot, including signature padding.
- Recovery/security configuration audit passes for the inspected build.
- Chromium console checks pass at 1440, 390 and 320 px in dark/light themes, without horizontal overflow or external asset requests. They cover keyboard skip access, literal log rendering, login errors, updater reconnection after a simulated connection loss, and backend failure reporting. Screenshots were visually inspected at desktop and mobile sizes.
- These browser checks use mocked API responses; actual board/browser end-to-end behavior and mobile certificate enrollment remain separate checks.

## Attached-board findings

- USB boot traces for rc.1 and rc.2 reach Wi-Fi AP+STA mode, DHCP at 192.168.4.1 and HTTPS listening on port 443.
- Radio read-back confirms hotspot `203-C5D474`, channel 1, non-hidden.
- Despite this, neither the Mac nor the user's phone discovered the hotspot at the initial radio setting. The Mac's connection attempt reported “Could not find network”; the initial connection attempts could not reach HTTPS. This was resolved by the later radio setting described below.
- rc.2 USB health samples show services running and stable settled free heap of 181,884 bytes over the observed short interval. This is not the 60-minute soak gate.
- Setting the maximum transmit power to 8.5 dBm made the hotspot discoverable; the user confirmed it appeared on their phone. The Mac then completed certificate-pinned HTTPS login and status reads.
- The first real OTA cycle passed: rc.3 in ota_0 → rc.4 in ota_1, full image/signature verification, updater ticket reconnection and confirmation at 30.693 seconds after boot. The existing administrator/hotspot credentials remained usable, settings were healthy, and rollback became available.
- Later firmware refinements still require the repeated-cycle and soak gates; this first cycle is not a complete stability claim.
- Real-board checks rejected 27 authentication, CSRF, concurrency, bounds, transfer, compatibility and signature cases, including a complete image signed with a disposable wrong RSA key. The running rc.5 image and saved settings were retained.
- Actual Chromium desktop/mobile viewport checks completed a browser OTA to rc.6. Serial monitoring then caught an idle-task watchdog reset during successive TLS handshakes and password verification. This invalidates a stability claim for that revision, even though the browser showed successful installation. The decoded stack was in the TLS elliptic-curve handshake. Explicit scheduling yields at TLS-session and HTTP-request boundaries are being qualified; neither watchdog is disabled. The repeated-cycle gate will use rebuilt images after this correction.
- Hotspot-only management with an unavailable configured uplink passed, and the supplied uplink was restored. No physical power removal occurred during the first cue window; that expired attempt does not count as a power-loss test.
- The scheduling correction completed 12 successive fresh Chromium sessions and password checks without a reset. Settled status-response heap stayed around 100 KiB; the minimum during concurrent TLS activity was 35,592 bytes. One earlier stress attempt stopped on an intermittent browser timeout after eight sessions without a board reset. Browser login now allows 45 seconds for TLS and the password derivation, displays its pending state, and passes a controlled 25-second-response regression. Final repeated-cycle/soak acceptance remains open.
- The subsequent post-OTA browser test reproduced idle-watchdog starvation, this time inside password derivation while startup release work was also active. The earlier boundary-yield correction was insufficient and is not qualified. The HTTPS and release workers now use ESP-IDF's time-sliced idle priority so long crypto operations share CPU with IDLE; health and radio tasks retain higher priority. CLI login also permits the bounded 45-second authentication budget. These changes require fresh qualification images and startup-load verification.
- With time-sliced workers, two consecutive post-OTA startup browser runs completed on rc.71 and rc.72 without an unexpected reset. The board then downloaded and verified the actual GitHub manifest, signature and rc.74 image over HTTPS. Browser refresh during that installation resumed the same transaction, and rc.74 confirmed healthy. The public rc.74 release remains a qualification prerelease.
- A later local-upload request exceeded its 20-second reply budget while rc.74 remained healthy. Clients now resolve the ticket's accepted offset before any retry. This correction and the requirement for recent updater contact at the health boundary are covered by host tests; the final hardware qualification uses rebuilt images containing them.

## Current qualification results

The latest firmware implementation is source commit `d0dd930fc647f30a433515dae19160cac20ddb75`. Signed qualification image `0.1.0-rc.99` is published as a **prerelease**, not a stable release. The fault images use explicit build flags and are not published as production firmware.

- Linux CI passed both the firmware and console jobs for this source revision: [run 42909109762](https://github.com/Achxy/203-entrance/actions/runs/42909109762).
- All **28 real-board rejection checks** passed on rebuilt rc.75, including a full-length incorrect administrator password, CSRF/origin failures, concurrent starts, interrupted-transfer timeout, size/offset rules, compatibility, downgrade, damaged digest, unsigned image, wrong RSA key, unavailable rollback and throttling. Specific rejection reasons were checked; transport failures were not counted as signature rejections. The running image and settings generation were unchanged.
- Hotspot-only management with an unavailable configured uplink passed again on rc.75. The optional uplink was restored afterward.
- An actual browser upload from rc.75 to rc.76 deliberately lost one successful chunk acknowledgement. The browser queried the accepted offset, recovered, and confirmed the expected image. Actual HTTPS console checks passed at 1440, 390 and 320 pixels after reboot, with no external assets or page errors. This is desktop Chromium at mobile viewport sizes; physical-phone certificate enrollment remains unverified.
- **Physical power interruption during an unconfirmed trial passed.** The user unplugged/reconnected USB while rc.77 was pending. Continuous serial and USB-presence evidence established that removal preceded the 120-second deadline; rc.76 returned with a power-on reset, healthy services and retained credentials/settings. macOS assigned a different USB port after reconnect; the same hardware serial was verified. An initial 20-second polling window ended before removal, so the result was established from the continuous recordings and subsequent authenticated recovery, rather than treating that expired polling attempt as a pass.
- **Physical power interruption during transfer passed.** USB removal occurred while writing the inactive slot. The running rc.76 returned with a power-on reset, unchanged slot and preserved settings/credentials.
- **Physical interruption after verification and boot selection passed.** A qualification-only activation pause held rc.78.1 before reboot; the user cut power about 60.7 seconds after the stage was armed. The selected rc.79 booted, reconnected and confirmed healthy with retained settings/credentials. This establishes post-selection/pre-first-boot recovery, not interruption of a particular metadata write instruction.
- Expired cue attempts are excluded. One earlier activation cue resulted in removal during the subsequent trial, so it was not counted as an activation-stage pass. The fault-only pause was extended from one minute to five minutes in `f2977b6`; ordinary production code retains its two-second delay. Normal qualification images built from `d0dd930` are unaffected by this preprocessor-only test-fixture change.

## Remaining acceptance gates

| Gate | Evidence still required |
|---|---|
| Trial/reset faults | Separate missing and stale confirmation, failed startup, deliberate health watchdog, reset during trial and explicit rollback checks |
| Remote release on final implementation | Actual download/confirmation of the rebuilt candidate; the earlier rc.74 GitHub flow passed before the latest acknowledgement/contact changes |
| Stability | 20 alternating OTA cycles and a 60-minute soak without unexplained resets, progressive heap loss, corrupted settings or management loss |
| Final delivery | Qualified firmware installed, recovery bundle verified, stable release published and final hardware security state checked |

Until these remaining gates are measured, the implementation must not be described as fully stable or completely hardware validated.
