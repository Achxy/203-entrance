# Validation record

Status: **qualification in progress; stable acceptance has not been claimed**.

## Hardware and preservation

- Attached Espressif ESP32-C3 QFN32 revision 0.4, 4 MB embedded XMC flash, 40 MHz crystal, USB Serial/JTAG.
- Original full flash saved before writing; 4,194,304 bytes, SHA-256 `a0f6eb5c4d053e689d5dcd3723e0908319e4d5fcf067344cbfe001b2c639e1db`.
- A second full pre-install backup completed with metadata under private `.local/backups/`.
- Signed initial firmware and unique identity were installed over USB. Bootloader, partition geometry, native signature and flash-write hashes were checked.
- Hardware Secure Boot and flash encryption were disabled during inspection. No eFuse write command has been used. A final security-state read remains part of acceptance.

## Automated evidence

- 30 host test methods pass, with additional subcases: actual C SemVer/compatibility/CRC/hex logic, actual C request/manifest parser, authentication/throttling/CSRF policy, copy-on-write settings selection and schema compatibility, RSA manifest verification and wrong-key/tampered/truncated cases.
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

## Acceptance gates still open

| Gate | Evidence required |
|---|---|
| Hotspot management | Successful association, certificate-pinned HTTPS login/status and browser interaction |
| Real OTA | Signed local upload, candidate verification, reboot and healthy confirmation |
| Authentication/CSRF/concurrency on board | Expected rejections with the current image/settings retained |
| Credentials/settings | Preserved across update and explicit/automatic rollback |
| Native image rejection | Wrong signer, unsigned, corruption, truncation, oversized and incompatible images |
| Startup/reset faults | Failed startup, missed confirmation, watchdog and reset during trial |
| Remote release | Actual HTTPS GitHub manifest/image download through a temporary 2.4 GHz Internet uplink |
| Actual power interruption | Physical unplug or switched power during transfer, activation and trial; separate from software reset |
| Stability | 20 alternating OTA cycles and 60 minutes with no unexplained resets, progressive heap loss, corrupted settings or loss of management |
| Final delivery | Qualified firmware installed, recovery bundle verified, source/release published and final hardware security state checked |

Until these gates are measured, the implementation must not be described as fully stable or completely hardware validated.
