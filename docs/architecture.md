# Architecture and invariants

The product is a reusable OTA foundation for an ESP32-C3 Super Mini, revision 0.4, with 4 MB flash. It has no entrance-control behavior yet. ESP-IDF is pinned by commit in `toolchain.json`; cJSON is pinned by `dependencies.lock`. The modules are native C, built by CMake.

| Module | Responsibility |
|---|---|
| `network.c` | Persistent WPA2 hotspot, optional station uplink, reconnect backoff and SNTP |
| `store.c` | USB-provisioned identity, password verification, settings snapshots and update journal |
| `ota.c` | Single-writer streaming update, verification, activation, confirmation and rollback |
| `health.c` | Independent trial deadline, watchdog, continuous health checks and BOOT maintenance |
| `api.c` / `auth.c` | HTTPS, authentication, CSRF protection, bounded requests and versioned API |
| `release.c` | Public GitHub release checks, HTTPS policy, manifest verification and remote transfer |
| `protocol.c` / `core.c` / `settings_policy.c` | Portable parsing, compatibility, SemVer, CRC and constant-time comparison |
| `diagnostics.c` | Bounded events, memory/reset/slot status |
| `display.c` / `media.c` / `builtin_loop.c` / `captions.c` | ST7735R SPI task, bounded ECL1 playback, fixed room header, persistent media and panel controls |
| `web/` / `tools/` | Offline browser console and certificate-pinned CLI using the same API |

## Flash layout

| Partition | Offset | Size | Purpose |
|---|---:|---:|---|
| nvs | `0x9000` | `0x7000` | Versioned settings snapshots |
| otadata | `0x10000` | `0x2000` | ESP-IDF redundant boot selection |
| identity | `0x12000` | `0x6000` | Read-only credentials, HTTPS identity, manifest public key |
| state | `0x18000` | `0x8000` | Update journal plus quota-limited `media_v1` clip/config |
| ota_0 | `0x20000` | `0x1E0000` | Signed application A |
| ota_1 | `0x200000` | `0x1E0000` | Signed application B |
| coredump | `0x3E0000` | `0x20000` | Crash capture |

The bootloader starts at zero and the partition table at `0x8000`. OTA writes neither. Both application slots include all dashboard assets and the complete RSA signature sector. Build and release checks enforce the final signed size, including padding. There is no separate factory application.

Display content is capped at 12 KiB, reserving space for two transactional NVS
blob generations, the update journal, panel configuration and garbage collection.
The display uses fixed buffers and a low-priority task; its heartbeat and SPI
health participate in trial checks. Playback runs during trial, freezes during
OTA transfer/activation, and resumes after an abort. Content changes share the
operation mutex and cannot write flash during OTA. See [display details](display.md).

## Update transaction

1. An authenticated administrator starts an upload or verified release installation. A mutex admits one transfer. Settings changes and explicit rollback conflict with an active transfer or trial.
2. A random 256-bit ticket is returned only to that updater. The engine keeps its SHA-256 digest. Chunk offsets must exactly match the bytes accepted; each chunk is at most 8192 bytes. If a reply is lost, clients query that ticket's accepted offset before retrying, with at most three attempts. An expired or aborted transaction must restart from the beginning.
3. The engine inspects the image/app/custom descriptors, checks ESP32-C3, project, board, partition-layout and settings compatibility, and requires a strictly newer SemVer. For a release download the embedded version must equal the signed manifest version.
4. Writes go exclusively to the inactive slot. A streaming digest covers the entire signed image. `esp_ota_end` verifies the native ESP-IDF RSA-3072 signature. Only then is the durable ticket/candidate journal committed, followed by boot selection.
5. The explicitly requested installation reboots after its response. The bootloader marks the candidate pending verification. The application compares its complete image digest with the journal before accepting the ticket.
6. Confirmation requires both an updater request with the one-time candidate ticket and 30 continuous seconds of healthy local management, valid settings and available heap. Confirmation must occur before 120 seconds from boot. A reset during an unconfirmed trial causes bootloader rollback; a missed deadline requests rollback explicitly.
7. ESP-IDF boot metadata is authoritative across a reset between marking the image valid and saving the confirmed outcome. The old image reports a rolled-back transaction using the same scoped ticket, allowing the initiating client to report failure accurately.

The ticket cannot log in, change settings, or start another update. Confirmation is idempotent for its transaction. The updater must keep reconnecting: the last confirmation request must be at most ten seconds old when the continuous 30-second health window completes. An early acknowledgement alone cannot confirm a trial that subsequently becomes unreachable. Sessions are lost at reboot; the ticket supports reconnection without storing the administrator password in the browser. A new transaction replaces the ticket.

A transfer aborts after 30 seconds without data or 300 seconds total. Remote connection attempts are bounded; interrupted transfers must restart. Boot selection is untouched on parsing, write, digest, signature, or journal failure. A full signature check can take appreciable time on the C3.

## Settings and maintenance

Identity lives separately from settings and updates. `cfg_v1` contains two CRC-protected snapshots with increasing generations; a write targets the older snapshot and commits before changing RAM state. A torn replacement retains the previous valid snapshot. Unknown/corrupt storage is never automatically erased. Future incompatible settings schemas must use a new namespace, copy forward transactionally and retain `cfg_v1` until older firmware is no longer a recovery target. Version 1 deliberately supports only schema 1.

The Wi-Fi driver starts with its own NVS access disabled; application-owned configuration is applied from RAM. This is essential for maintenance: a failed settings partition must not prevent the separate identity partition from supplying the hotspot credentials and HTTPS identity. PHY calibration can fall back to full calibration when its cached data is unavailable. The `storage` qualification fault reproduces an unavailable default NVS partition without erasing its saved contents.

The hotspot starts independently of station success. During trial boot, station association is deferred until confirmation plus an observation grace of up to 10 seconds, or 45 seconds from boot, whichever policy path applies first. The bounded fallback leaves time for an updater reachable only over the station to reconnect before the unchanged 120-second deadline. Settings and credentials are not rewritten for this pause. Hold BOOT for five seconds while running to cancel a transfer and disable the uplink until reboot. This does not erase credentials or bypass login. A corrupt identity requires USB reprovisioning. If no valid previous image exists, rollback is refused and USB recovery remains available.

## Security boundary

The application requires standard Secure Boot v2 RSA-3072 signatures through ESP-IDF's software verification mode. The initial USB installer independently verifies the signed image before writing it. Hardware Secure Boot, flash encryption and eFuse anti-rollback are disabled. No eFuses are burned. Physical access can read credentials or replace the bootloader/application; this version protects the OTA path, not against a physical attacker. Signing-key replacement therefore requires USB reprovisioning.

HTTPS uses a unique device certificate and private key provisioned over USB. The CLI verifies the enrolled certificate and pins its SHA-256 fingerprint. Browser enrollment is a separate documented trust step. Administrator passwords use PBKDF2-HMAC-SHA-256 with a unique salt; sessions use random tokens, Secure/HttpOnly/SameSite cookies and CSRF headers. Invalid login attempts are throttled globally. No password, ticket, private key, or bearer token is written to application events.

Release downloads validate HTTPS certificates, hostname and certificate time against the full bundled trust store. The build audit requires cross-signed bundle verification and `MBEDTLS_HAVE_TIME_DATE`, and rejects insecure/weak-verification options. Earlier RC12 and older configurations omitted the expiry-check option; the corrective validation is recorded in [TLS qualification](arcade/tls-qualification.md). The board waits for an uplink and a plausible SNTP clock. Redirects are bounded and restricted to the project release path and GitHub asset hosts; there is no arbitrary URL fetch API. A signed manifest authenticates the image digest, size, version, board, layout and settings schema. No GitHub token is provisioned on the board. A compromised time source can cause availability problems; image authenticity still depends on the project signing key.

Reference: [ESP-IDF OTA](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/system/ota.html) and [software signed-app verification](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/security/secure-boot-v2.html#signed-app-verification-without-hardware-secure-boot).

## Scheduling under cryptographic load

The HTTPS server and release worker run at the idle task priority with ESP-IDF time slicing enabled. This gives IDLE a CPU share even inside a long PSA password derivation or TLS operation. The independent health task and radio tasks retain higher priority. Both the idle watchdog and the 15-second health watchdog remain enabled. Authentication clients allow 45 seconds for a bounded login; ordinary requests keep shorter deadlines.

The built-in Neko animation uses `builtin_loop.c`, the bounded ECL1 decoder and
`captions.c`; the historical NES decoder is not linked into the current image.
Pixel strips use blocking DMA completion, letting HTTPS/IDLE run while the SPI
peripheral transmits. Tiny panel commands remain synchronous polling operations.
TLS uses dynamic record buffers, with the standard 16 KiB incoming limit intact,
so idle browser sockets do not each retain a complete TX/RX buffer pair. Server
certificate/key configuration is retained for subsequent handshakes. The build
audit enforces dynamic buffers after live qualification found low heap margin.
