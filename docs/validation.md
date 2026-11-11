# Validation record

**Qualification is in progress; no stable release has been declared.** The user expanded the OTA foundation to include an ST7735R, persistent short clips and an autonomous full-game scene replay. Combined hardware checks and a continuous operating soak remain required. Public `0.1.0` and `0.1.1` assets are prereleases; `0.1.0` is explicitly superseded because qualification found a maintenance-startup defect.

## Hardware and preservation

Two ESP32-C3 Super Mini boards, revision 0.4, 4 MB XMC flash, 40 MHz crystal and native USB Serial/JTAG have been provisioned. The original board supplied the display observations below. The second board was flashed before its display was connected. Display wiring is documented in [display.md](display.md). The microSD interface is not connected.

The original board's full flash was saved before installation: 4,194,304 bytes, SHA-256 `a0f6eb5c4d053e689d5dcd3723e0908319e4d5fcf067344cbfe001b2c639e1db`. The second board has its own pre-install backup and matching metadata. Each received a unique identity and signed baseline over USB. No eFuse write command has been used. The second-board readback, security-state comparison and private recovery bundle are described below.

## Measured OTA and security evidence

- **28 real-board rejection checks passed** on rebuilt `0.1.0-rc.75`: authentication, CSRF/origin, concurrent starts, interrupted-transfer timeout, bounds/offsets, compatibility, downgrade, damaged digest, unsigned image, wrong RSA key, unavailable rollback and throttling. Specific rejection reasons were checked; a connection failure was not counted as signature rejection. Active firmware and settings were retained.
- Hotspot-only HTTPS management with an unavailable uplink passed. The uplink was restored. An 8.5 dBm radio-power limit resolved the initial hotspot discovery failure; the user confirmed visibility on their phone.
- Actual GitHub manifest, signature and image download passed, including browser refresh during installation and confirmation of the expected version.
- An actual browser upload deliberately lost one successful chunk response. The browser queried the accepted offset, avoided duplicating the write, and confirmed `0.1.0-rc.76`. Certificate-pinned HTTPS console checks passed at 1440, 390 and 320 pixels with local assets and no page errors.
- **Three user-confirmed physical power interruptions passed:** during inactive-slot transfer, during unconfirmed trial, and after verification/boot selection but before the candidate's first boot. Transfer/trial interruptions recovered prior firmware; the selected candidate booted and confirmed after the activation interruption. Credentials/settings survived. Expired cues were excluded. The activation test does not establish interruption of a particular boot-metadata write instruction.
- **Six software fault/recovery checks passed:** missing confirmation, stale early confirmation, failed startup despite fresh contacts, reset during trial, deliberate health-task watchdog, and explicit rollback. Serial evidence distinguished watchdogs from software restarts.
- An unavailable-settings-NVS fault initially prevented Wi-Fi maintenance startup. Disabling driver-owned NVS fixed it: actual hotspot and certificate-pinned HTTPS maintenance login/status passed, unhealthy confirmation was refused, and rollback preserved settings and credentials. The test did not erase the settings partition.

The corrected OTA-only cycle series **completed all 20 alternating updates**, using `0.1.1-rc.10` through `rc.29`, with the last installation through GitHub. Settings generation 6 was preserved. Earlier cycles before the storage correction remain historical evidence. This is foundation evidence and does not establish qualification of later display scheduling or replay changes.

## Display and replay evidence, 11 November 2026

- The user physically confirmed the RGB test card, corners, grayscale ramp and later uniform lossless animation. The correct panel profile is black/RGB. The first Mario preview was also physically confirmed; subsequent requested changes selected landscape, larger text, a yellow divider and two seconds per message.
- Eleven real-board custom-media cases passed, including 24 full-capacity 12,288-byte replacements, binary hash readback, malformed/truncated/oversized rejection, interrupted-body preservation and OTA conflicts.
- Signed preview versions `0.2.0-rc.4` through `rc.8` were installed over USB with full pre-install backups, inactive-slot writes, signature/readback verification and 30 continuous healthy seconds. These are **USB installation checks, not OTA trial-confirmation checks**.
- The initial idle-priority replay ran about 7.7 frames/second. A bounded task one priority above IDLE, guaranteed sleeps and retained frame deadlines sustained about 10 frames/second on `rc.8`. Observed settled free heap was 134,808 bytes, with no reset in that measured interval. This short observation is not a 60-minute soak or a concurrent HTTPS-load test.
- The complete asset contains all 32 levels and the ending: **16,500 frames, 1,647.294 seconds, 693,570 bytes**. Offline capture compared all 16,484 sampled 256×240 palette-index rasters with the shipped C renderer, with zero mismatches. The C decoder also reproduced all 16,500 packed scenes and wrapped correctly. Exact source hashes and reproducible evidence are in [replay provenance](../assets/replay-provenance.json).
- No ROM program code or emulator runs on the board. The controller sequence is locally planned, favors walking, traverses every level, and is replayed continuously from power-on. Imported speedrun/TAS movies were excluded.

The Mac currently retains its Internet network; the previously shared phone uplink is unavailable. USB work continues without joining the ESP's offline hotspot. Actual combined-firmware HTTPS/OTA tests require a reachable shared network and remain a separate gate.

## Second-board installation

The second board received the **exact published `0.2.0-rc.9` application**:
SHA-256 `b51e31f10a493f070cd406bed240f3e67647cfc0f8d332f0ec8c01b9b92c83ee`,
1,904,640 bytes including signature padding, leaving 61,440 bytes in each slot.
Its original 4 MB flash was saved before erasure. It received unique credentials
and a separate HTTPS identity using the existing project signing key.

Full-flash readback matched the application, bootloader, partition table,
identity and initial valid `ota_0` boot record. The inactive application remained
empty and the next USB-update plan selected `ota_1`. Reported security fields
were unchanged. Both initial and subsequent boots passed 30 continuous healthy
seconds with advancing replay counters. Original/installed full-flash backups,
credentials and installation evidence remain private and associated with that board.

The initial installation checks above were performed **without an attached
display**. The user subsequently soldered the panel and confirmed visible
operation after supplying LED+ separately from 3V3. This does not establish
complete loop operation, concurrent HTTPS/OTA load or a 60-minute combined
operating soak. There is no valid previous application on this fresh board
until an update populates the other slot.

### Qualification checkpoint, 11 November 2026

A fresh full-flash backup was verified against its metadata and retained the
same identity and working RC9 application. The next USB installation stopped
before writing because the board no longer enumerated. A further 15-minute wait
did not recover the connection; the cause is unconfirmed. **No newer firmware
was written and the 60-minute soak did not start in this attempt.**

Source `9e0ca758bcdc4203269e5b8b8393c06985d2662d` corrects the USB outcome
reporting defect below and strengthens the soak checks. Its 63 host tests,
desktop/mobile browser regressions and Linux firmware/browser CI passed.
Signed RC10 and RC39 qualification releases were published and downloaded;
their bytes, SHA-256 sums, signed manifests and native image signatures matched.
Private images and scripts are prepared for rejection/fault tests and 20
alternating OTA cycles. Preparation and host downloads are not device OTA passes.

Actual network checks require a shared reachable network. The Mac currently has
only a Wi-Fi Internet uplink; its sharing UI offers no Wi-Fi output for that
source. A separate Ethernet/USB Internet input would permit a Mac hotspot.
Physical power-cut tests on the second board were unavailable by the user's
response and are not inferred from software resets or the original board's tests.

## Defects found during qualification

Initial browser startup load caused idle-watchdog resets inside TLS/password derivation. Yields only at request boundaries were insufficient. HTTPS and release workers now run at time-sliced idle priority; health/radio retain higher priority and both watchdogs remain enabled. Subsequent startup browser checks passed. A later lost upload reply led to bounded accepted-offset recovery in both clients. Trial confirmation also requires updater contact within the preceding ten seconds at the healthy boundary. These findings are retained, not hidden by later successful installations.

Second-board qualification also found a reporting error: a previously recorded
`usb_installed` outcome changed to `rolled_back` on an ordinary restart. The
outcome is now preserved, with a production-state-machine regression covering
both the newer USB installation and its subsequent restart.

The soak command starts its duration at the first healthy sample, requires a
complete device/host time window, and rejects unhealthy/trial states, resets,
stale telemetry and a settled median heap decline greater than 2 KiB. Replay
qualification additionally requires advancing rendering and the requested loop
count. HTTPS checks also enforce unchanged settings generation, clip hash and
panel configuration. USB evidence alone does not establish HTTPS availability
or credential/settings persistence.

## Automated combined-firmware checks

- **63 host test methods pass**, with additional subcases: actual C compatibility/SemVer/CRC, request/manifest parsing, authentication, settings selection and OTA state transitions, plus Python RSA verification and client recovery. Media tests execute the actual C parser/decoder and compare the CLI parser over 1,500 deterministic mutations, all truncations, malformed runs, invalid colors, odd-pixel padding and size bounds. Replay tests cover packed deltas, block boundaries, seeks, loops, malformed scenes and index bounds; USB tests reject stale/reset/unhealthy/wrong-version boot evidence, allow bounded initial startup, verify the initial valid baseline record and preserve separate device profiles. Soak tests reject incomplete windows, resets, stalled replay, missing loop boundaries, settings changes and progressive heap loss.
- JavaScript ECL1 round trips, palette quantization, checksum and bounds pass.
- Mocked browser regressions pass at 1440/390/320 pixels. The display editor additionally passes both themes, real browser WebM decoding, message conversion, preview, save hash readback, lost acknowledgement, pause, download, removal, invalid clips and duration limits. Replay controls, keyboard activation, landscape preview, the yellow divider and all three captions in a two-second cycle pass. All six portrait/landscape caption renderings were compared pixel-for-pixel between C and the browser. Screenshots were inspected; no horizontal overflow or external assets were observed.
- Signed display candidate `0.2.0-rc.1` is 1,183,744 bytes including signature padding, leaving 782,336 bytes in each unchanged application slot. Native signature verification and recovery-policy checks pass.

Mocked browser APIs and native flash/signature doubles do not prove actual storage, electrical behavior, power resilience or performance. Those are separate hardware gates. A driver reporting SPI success cannot prove that the write-only panel shows pixels.

## Remaining combined-firmware gates

| Gate | Required evidence |
|---|---|
| Display hardware | Full replay on an attached display, both loop boundaries and actual browser controls under replay load; second-board USB installation has passed |
| Media storage | Combined replay/custom-mode persistence across OTA and rollback; earlier full-capacity/rejection cases have passed |
| Combined regression | Actual browser controls and signed updates under display load |
| Stability | Alternating-cycle evidence plus 60 continuous minutes without unexplained resets, heap loss, corrupted settings or management loss |
| Delivery | Clean source/tag, passing CI, signed public release, installed final image and verified private USB recovery bundle |

The original board supplied the detailed color/card observations; the second board has USB evidence and the user's subsequent confirmation of working display output. Phone-sized layouts use desktop Chromium; physical-phone certificate enrollment remains unverified. Original OTA power-cut/authentication/fault evidence can be carried forward only with its component scope stated. New display/storage behavior requires its own evidence.
