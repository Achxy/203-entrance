# 203 Entrance · ESP32-C3 OTA foundation

A local developer console and signed OTA update system for an ESP32-C3 Super Mini with 4 MB flash. The device hosts a unique password-protected Wi-Fi network. Optional station Wi-Fi enables public GitHub release downloads; local management needs no Internet connection.

**Qualification is in progress.** Consult [the hardware validation record](docs/validation.md) for measured results and open acceptance gates. A successful build or simulated test does not establish hardware stability.

## What is included

- Native ESP-IDF v6.1, pinned to `fff9895c82d744c7237be8847347bdd1b07c6643`, with separate networking, storage, update, health, diagnostics and HTTPS management modules.
- Two application slots, RSA-3072 signed ESP-IDF images, signed release manifests, strict compatibility/version checks and bounded-memory transfers.
- Trial boot with 30 continuous healthy seconds, updater reconnection and a 120-second confirmation deadline. Explicit rollback, BOOT maintenance and USB recovery.
- Unique USB-provisioned administrator credentials and HTTPS identity, certificate-pinned CLI, browser sessions, CSRF protection and authentication throttling.
- Offline dark/light console, network controls, bounded events, diagnostic export, developer CLI, native policy tests and responsive browser tests.
- ST7735R display studio: local image/video conversion, message rendering, persistent short loops, playback controls, and panel calibration using the supplied wiring. See [display setup and storage limits](docs/display.md).
- Built-in NES scene replay, independent two-second captions, landscape output and a yellow divider. The offline recorder verifies sampled pixels against an emulator; the ESP runs a bounded scene decoder. See [replay content and reproduction](docs/replay.md).
- Public source in [Achxy/203-entrance](https://github.com/Achxy/203-entrance) and separate signed assets in [Achxy/203-entrance-firmware](https://github.com/Achxy/203-entrance-firmware).

Hardware Secure Boot, flash encryption and eFuse anti-rollback stay disabled. Software signature verification protects the OTA path while keeping USB recovery. Physical access can read or replace flash. No entrance-device behavior is implemented yet.

## Quick start on macOS

Install Git, CMake, Ninja and Python 3.12 from their official package sources. With Homebrew:

```sh
brew install cmake ninja python@3.12
./entrance setup
./entrance doctor
./entrance build --version 0.1.0-rc.1
./entrance test
./entrance audit
```

Setup keeps the pinned SDK and its tools under ignored `.tools/`. It creates a project signing key once under protected, ignored `.local/keys/`. **Back up that key before deploying any device.** Keep `.local/` private; it includes credentials and complete flash backups.

For an initial USB installation:

```sh
./entrance flash --port /dev/cu.usbmodem1101
```

For another board, give it a separate private device profile. The same signed
firmware and project signing key serve every compatible board; each device gets
its own hotspot, administrator credentials and HTTPS identity:

```sh
./entrance flash --port PORT --device-profile .local/devices/BOARD/device.json
./entrance status --device-profile .local/devices/BOARD/device.json
```

Device commands accept `--device-profile`; the default remains `.local/device.json`.
Keep each board's profile and certificate files together. Selecting the wrong
existing profile is rejected before flashing or provisioning that identity.

The installer checks chip/flash geometry and security state, backs up all 4 MB, verifies the application signature, then installs the baseline and unique identity. It does not burn eFuses. This is an initial/recovery operation: it replaces flash contents. Existing profile credentials are reused; application settings are initialized. Use OTA for routine updates.

Initial installation writes an explicit valid `ota_0` boot record. The next USB
update can safely identify its inactive slot, and the first OTA trial has a
previous baseline record. The other application slot remains empty until an update.

1. Open `.local/device.json` locally to find the generated hotspot and administrator passwords. Do not paste this file into an issue or commit it.
2. Join the `203-XXXXXX` hotspot. Its console is `https://192.168.4.1`.
3. Verify and enroll the provisioned certificate as described below, then sign in. Wait for the startup health check before installing an update.
4. For CLI management, run `./entrance status`. It uses the saved profile, trusted certificate and explicit certificate fingerprint pin.

To deploy a newer signed image:

```sh
./entrance build --version 0.1.0-rc.2
./entrance upload build/203-entrance.bin
```

Keep the CLI or initiating browser tab connected through reboot. Deployment succeeds only when the expected firmware reports **confirmed healthy**. No bootloader or partition-table replacement occurs over OTA.

When USB is available, a settings-preserving physical update is also supported:

```sh
./entrance usb-install build/203-entrance.bin --port /dev/cu.usbmodem101
```

This verifies both signatures and the exact partition layout, backs up all flash,
writes the inactive application slot and alternate boot record, and checks 30
continuous healthy seconds over serial. It requires a newer signed version.
It retains credentials, custom clips, the previous application, bootloader and
partition table. This trusted physical installation does not exercise the OTA
trial-ticket protocol. USB monitoring keeps the Mac on its Internet connection;
Bluetooth management is not implemented.

## Browser certificate enrollment

USB provisioning creates a unique certificate under `.local/devices/<device>/device-cert.pem`. Compare its SHA-256 with `certificate_sha256` in the private profile:

```sh
openssl x509 -in .local/devices/DEVICE/device-cert.pem -noout -fingerprint -sha256
```

On macOS, import that exact public certificate into Keychain Access, open its Trust settings and explicitly trust it for SSL. Use `https://192.168.4.1`, which is included in its subject alternative names. Other browsers/platforms may use a separate certificate store or a per-site exception; verify the same certificate fingerprint before enrolling it. Never disable certificate validation globally. Mobile certificate-enrollment behavior must be verified on the intended phone/browser.

The certificate is retained across OTA and rollback. A changed fingerprint requires investigation or intentional USB reprovisioning. The CLI's pin remains mandatory even when connecting to a different station IP with `--host`.

## Common commands

| Command | Purpose |
|---|---|
| `setup`, `doctor`, `build`, `test`, `audit` | Repeatable toolchain, build and policy checks |
| `backup`, `flash`, `usb-install`, `provision`, `monitor` | USB installation, identity and recovery |
| `status`, `upload`, `rollback`, `diagnostics` | Local management and confirmed deployment |
| `display`, `media` | Panel controls and verified persistent clip upload/download |
| `network --ssid NAME`, `network --disable` | Optional uplink; password is prompted privately |
| `check`, `install` | Verify and install a public GitHub release |
| `keygen`, `sign`, `release --publish` | Local production signing and release publication |
| `soak --minutes 60` | Management API stability samples |
| `soak --serial --minutes 60 --port PORT` | USB health samples while keeping the host online |

Run `./entrance COMMAND --help` for options. `build --fault startup|watchdog|reset|activation|storage` creates an explicit qualification image; publication rejects these builds. The `storage` test leaves saved settings intact but makes their NVS partition unavailable to that image, exercising HTTPS maintenance and trial rollback. `upload --no-confirm` exercises the missed-confirmation rollback path.

## Development and documentation

Use Node 24 for development-only browser checks:

```sh
npm ci --ignore-scripts
npx playwright install chromium
npm test
```

No npm code, external fonts, CDN assets or Internet resources are required by the device console. Screenshots and browser evidence are written under `.local/browser/`. Linux CI builds with a disposable signing key and exercises native parsing/state tests and browser checks; it never holds the production key.

- [Architecture, partitions and security boundary](docs/architecture.md)
- [API v1](docs/api.md)
- [Display studio, wiring, clip format and storage](docs/display.md)
- [Recovery and signing-key backups](docs/recovery.md)
- [Release workflow](docs/releases.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Hardware validation and remaining gates](docs/validation.md)
