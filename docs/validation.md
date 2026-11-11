# Validation record

**Qualification is in progress; no stable release has been declared.** The user expanded the OTA foundation to include an ST7735R and persistent short clips. Combined hardware checks and a continuous operating soak remain required. Public `0.1.0` and `0.1.1` assets are prereleases; `0.1.0` is explicitly superseded because qualification found a maintenance-startup defect.

## Hardware and preservation

One ESP32-C3 Super Mini, revision 0.4, 4 MB XMC flash, 40 MHz crystal and native USB Serial/JTAG is under test. Display wiring is documented in [display.md](display.md). The microSD interface is not connected.

Original full flash was saved before installation: 4,194,304 bytes, SHA-256 `a0f6eb5c4d053e689d5dcd3723e0908319e4d5fcf067344cbfe001b2c639e1db`. A second pre-install backup has separate matching metadata and is not assumed identical. Unique identity and the signed baseline were installed over USB. No eFuse write command has been used. Final readback, security-state comparison and the private recovery bundle remain delivery gates.

## Measured OTA and security evidence

- **28 real-board rejection checks passed** on rebuilt `0.1.0-rc.75`: authentication, CSRF/origin, concurrent starts, interrupted-transfer timeout, bounds/offsets, compatibility, downgrade, damaged digest, unsigned image, wrong RSA key, unavailable rollback and throttling. Specific rejection reasons were checked; a connection failure was not counted as signature rejection. Active firmware and settings were retained.
- Hotspot-only HTTPS management with an unavailable uplink passed. The uplink was restored. An 8.5 dBm radio-power limit resolved the initial hotspot discovery failure; the user confirmed visibility on their phone.
- Actual GitHub manifest, signature and image download passed, including browser refresh during installation and confirmation of the expected version.
- An actual browser upload deliberately lost one successful chunk response. The browser queried the accepted offset, avoided duplicating the write, and confirmed `0.1.0-rc.76`. Certificate-pinned HTTPS console checks passed at 1440, 390 and 320 pixels with local assets and no page errors.
- **Three user-confirmed physical power interruptions passed:** during inactive-slot transfer, during unconfirmed trial, and after verification/boot selection but before the candidate's first boot. Transfer/trial interruptions recovered prior firmware; the selected candidate booted and confirmed after the activation interruption. Credentials/settings survived. Expired cues were excluded. The activation test does not establish interruption of a particular boot-metadata write instruction.
- **Six software fault/recovery checks passed:** missing confirmation, stale early confirmation, failed startup despite fresh contacts, reset during trial, deliberate health-task watchdog, and explicit rollback. Serial evidence distinguished watchdogs from software restarts.
- An unavailable-settings-NVS fault initially prevented Wi-Fi maintenance startup. Disabling driver-owned NVS fixed it: actual hotspot and certificate-pinned HTTPS maintenance login/status passed, unhealthy confirmation was refused, and rollback preserved settings and credentials. The test did not erase the settings partition.

The corrected OTA-only cycle series uses `0.1.1-rc.10` through `rc.29`, with the last installation through GitHub. Earlier cycles before the storage correction remain historical evidence. The completed foundation series and subsequent combined-display tests will be reported separately.

## Defects found during qualification

Initial browser startup load caused idle-watchdog resets inside TLS/password derivation. Yields only at request boundaries were insufficient. HTTPS and release workers now run at time-sliced idle priority; health/radio retain higher priority and both watchdogs remain enabled. Subsequent startup browser checks passed. A later lost upload reply led to bounded accepted-offset recovery in both clients. Trial confirmation also requires updater contact within the preceding ten seconds at the healthy boundary. These findings are retained, not hidden by later successful installations.

## Automated combined-firmware checks

- **44 host test methods pass**, with additional subcases: actual C compatibility/SemVer/CRC, request/manifest parsing, authentication, settings selection and OTA state transitions, plus Python RSA verification and client recovery. New media tests execute the actual C parser/decoder and compare the CLI parser over 1,500 deterministic mutations, all truncations, malformed runs, invalid colors, odd-pixel padding and size bounds.
- JavaScript ECL1 round trips, palette quantization, checksum and bounds pass.
- Mocked browser regressions pass at 1440/390/320 pixels. The display editor additionally passes both themes, real browser WebM decoding, message conversion, preview, save hash readback, lost acknowledgement, pause, download, removal, invalid clips and duration limits. Screenshots were inspected; no horizontal overflow or external assets were observed.
- Signed display candidate `0.2.0-rc.1` is 1,183,744 bytes including signature padding, leaving 782,336 bytes in each unchanged application slot. Native signature verification and recovery-policy checks pass.

Mocked browser APIs and native flash/signature doubles do not prove actual storage, electrical behavior, power resilience or performance. Those are separate hardware gates. A driver reporting SPI success cannot prove that the write-only panel shows pixels.

## Remaining combined-firmware gates

| Gate | Required evidence |
|---|---|
| Display hardware | Visible test card/colors/edges; actual playback and controls |
| Media storage | Full-capacity replacement, interrupted/rejected transfers, persistence across reboot and OTA/rollback |
| Combined regression | Actual browser controls and signed updates under display load |
| Stability | Alternating-cycle evidence plus 60 continuous minutes without unexplained resets, heap loss, corrupted settings or management loss |
| Delivery | Clean source/tag, passing CI, signed public release, installed final image and verified private USB recovery bundle |

Scope is one attached board. Phone-sized layouts use desktop Chromium; physical-phone certificate enrollment remains unverified. Original OTA power-cut/authentication/fault evidence can be carried forward only with its component scope stated. New display/storage behavior requires its own evidence.
