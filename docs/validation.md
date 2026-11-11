# Validation record

**Qualification is in progress; no stable release has been declared.** The user expanded the OTA foundation to include an ST7735R, persistent short clips and an autonomous built-in animation (initially Mario, now Neko by request). Combined hardware checks and a continuous operating soak remain required. Public `0.1.0` and `0.1.1` assets are prereleases; `0.1.0` is explicitly superseded because qualification found a maintenance-startup defect.

The scope now also includes phone-controlled games, the public site, Clerk
identity/guest records and an Internet relay. See the [arcade checkpoint](arcade/validation.md)
for the installed RC9 image, production web checks, campus relay timeout and
the explicit prohibition on further laptop Wi-Fi switching.

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

The earlier USB-only period ended when the second board was permanently installed. Subsequent hotspot access and actual OTA evidence are recorded below; shared-network testing remains a separate gate.

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
did not recover the connection. The user subsequently confirmed that the board had been permanently installed elsewhere. **No newer firmware
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

- **65 host test methods pass**, with additional subcases: actual C compatibility/SemVer/CRC, request/manifest parsing, authentication, settings selection and OTA state transitions, plus Python RSA verification and client recovery. Media tests execute the actual C parser/decoder and compare the CLI parser over 1,500 deterministic mutations, all truncations, malformed runs, invalid colors, odd-pixel padding and size bounds. Replay tests cover packed deltas, block boundaries, seeks, loops, malformed scenes and index bounds; USB tests reject stale/reset/unhealthy/wrong-version boot evidence, allow bounded initial startup, verify the initial valid baseline record and preserve separate device profiles. Soak tests reject incomplete windows, resets, stalled replay, missing loop boundaries, settings changes and progressive heap loss.
- JavaScript ECL1 round trips, palette quantization, checksum and bounds pass.
- Mocked browser regressions pass at 1440/390/320 pixels. The display editor additionally passes both themes, real browser WebM decoding, message conversion, preview, save hash readback, lost acknowledgement, pause, download, removal, invalid clips and duration limits. Replay controls, keyboard activation, landscape preview, the yellow divider and all three captions in a two-second cycle pass. All six portrait/landscape caption renderings were compared pixel-for-pixel between C and the browser. Screenshots were inspected; no horizontal overflow or external assets were observed.
- Signed display candidate `0.2.0-rc.1` is 1,183,744 bytes including signature padding, leaving 782,336 bytes in each unchanged application slot. Native signature verification and recovery-policy checks pass.

Mocked browser APIs and native flash/signature doubles do not prove actual storage, electrical behavior, power resilience or performance. Those are separate hardware gates. A driver reporting SPI success cannot prove that the write-only panel shows pixels.

## Remaining combined-firmware gates

| Gate | Required evidence |
|---|---|
| Display hardware | Neko visibility on the attached panel, loop continuity and actual browser controls under animation load; second-board USB installation and earlier Mario visibility passed |
| Media storage | Combined replay/custom-mode persistence across OTA and rollback; earlier full-capacity/rejection cases have passed |
| Combined regression | Actual browser controls and signed updates under display load |
| Stability | Alternating-cycle evidence plus 60 continuous minutes without unexplained resets, heap loss, corrupted settings or management loss |
| Delivery | Clean source/tag, passing CI, signed public release, installed final image and verified private USB recovery bundle |

The original board supplied the detailed color/card observations; the second board has USB evidence and the user's subsequent confirmation of working display output. Phone-sized layouts use desktop Chromium; physical-phone certificate enrollment remains unverified. Original OTA power-cut/authentication/fault evidence can be carried forward only with its component scope stated. New display/storage behavior requires its own evidence.

## Remote access and Neko replacement, 11 November 2026

Certificate-pinned access to the second board's hotspot was established without
USB. On RC9 the dashboard loaded, but valid password verification took 66.3 s,
exceeding the supplied clients' 45 s login timeout. The pixel-transfer path was
polling for roughly 41 ms of each frame's wire time. Bulk strips now use blocking
DMA completion, releasing the CPU while retaining bounded polling for commands.

**Actual hotspot OTA of RC40 passed**, including inactive-slot upload, signature
verification, reboot, fresh ticket contact and healthy confirmation. RC9 remained
available as a valid rollback target. Valid login improved to 30.7 s including
TLS; observed rendering was approximately 10.3 fps. The Mac returned to its
Internet Wi-Fi automatically. This short measurement is not a one-hour soak.

The user then requested that the Mario artwork be replaced by a Neko paw wave.
The Neko asset is 10,582 bytes, 24 frames, eight poses, 10 fps and 2.4 seconds.
All frames were inspected; static body drift and the wrap transition are zero.
The production C decoder passed 100 complete loops against per-frame hashes.
All 144 full panel rasters (24 frames × two orientations × three captions)
matched between the C renderer and browser. The complete host suite has 65
methods; desktop/mobile conversion, keyboard, theme, offline asset and
acknowledgement-recovery regressions passed. See [Neko evidence](neko.md).

The private RC41 candidate is 1,183,744 bytes including RSA padding, leaving
782,336 bytes in each unchanged OTA slot. Initial installation was prevented
before any write when the hotspot disappeared; the user confirmed accidental
power-off and subsequently restored power. The retry and final publication
status are recorded separately below. No Neko operating-soak pass is inferred
from the earlier Mario uptime or original-board qualification.

The campus association took longer than the initial setup window. Later status
confirmed a station DHCP address, but a certificate-pinned connection from the
Mac to that address timed out, and the device clock remained unsynchronized.
Association alone is not proof of Internet access or peer reachability. The
management hotspot remains available. GitHub downloads and a continuous
online-host soak still need a reachable shared network. Actual second-board
power-cut qualification remains unavailable by the user's earlier response.

The powered-on retry **installed RC41 over the local hotspot and confirmed it
healthy**. Its embedded ECL1 readback matched SHA-256
`0f95e0adc4c32d7d6cc936444043c52602710330be2f0f78885b62a79ad35c9e`.
Settings generation 2 and panel configuration survived; RC40 remained a valid
rollback target. Login took 28.6 seconds including TLS. The measured rendering
counter advanced about 10.5 times/second (including caption-only redraws; the
animation period is 100 ms). This is a short installation check, not a soak.

Live browser testing on RC41 exposed insufficient TLS memory margin: minimum
free heap reached 2,240 bytes, and one page load did not show the display
controls. Subsequent controls passed, but that memory margin was unacceptable.
RC42 enables ESP-IDF dynamic TLS record buffers while retaining the full 16 KiB
incoming record limit, certificate verification and reusable server key/config.
Its signed hotspot update and healthy confirmation passed. With the live browser
open, minimum observed heap was **48,336 bytes**, with 128,304 bytes free across
the active-playback samples. There were no resets in that measured interval.

The actual certificate-pinned console passed at 1440/390/320 pixels in both
themes. Play/pause and keyboard activation worked; a paused paw retained its frame
while all three captions changed. After resuming, 12 samples spanning 22.35 s
showed 224 redraws and nine complete Neko loops, with all three captions and
healthy services. Playback was left running. The user explicitly confirmed
that **both the physical cat and text were moving again** after the update.
The earlier frozen screen report coincided with update/pause testing and is not
counted as an unexplained operating reset or a completed soak.

An initial three-sample caption test aliased the two-second cycle because each
request added latency; it was replaced by bounded frequent sampling. The raw
browser trace also recorded an aborted preview request despite successful preview
and control checks. The preview now cancels its download timer on completion;
final live verification remains necessary before treating that trace as resolved.
