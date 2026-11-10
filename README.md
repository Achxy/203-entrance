# 203 Entrance · ESP32-C3 OTA foundation

A local developer console and signed OTA update system for an ESP32-C3 Super Mini with 4 MB flash. The device hosts a unique password-protected Wi-Fi network. Optional station Wi-Fi enables public GitHub release downloads; local management needs no Internet connection.

**Qualification is in progress.** Consult [the hardware validation record](docs/validation.md) for measured results and open acceptance gates. A successful build or simulated test does not establish hardware stability.

## What is included

- Native ESP-IDF v6.1, pinned to `fff9895c82d744c7237be8847347bdd1b07c6643`, with separate networking, storage, update, health, diagnostics and HTTPS management modules.
- Two application slots, RSA-3072 signed ESP-IDF images, signed release manifests, strict compatibility/version checks and bounded-memory transfers.
- Trial boot with 30 continuous healthy seconds, updater reconnection and a 120-second confirmation deadline. Explicit rollback, BOOT maintenance and USB recovery.
- Unique USB-provisioned administrator credentials and HTTPS identity, certificate-pinned CLI, browser sessions, CSRF protection and authentication throttling.
- Offline dark/light console, network controls, bounded events, diagnostic export, developer CLI, native policy tests and responsive browser tests.
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

The installer checks chip/flash geometry and security state, backs up all 4 MB, verifies the application signature, then installs the baseline and unique identity. It does not burn eFuses. This is an initial/recovery operation: it replaces flash contents. Existing profile credentials are reused; application settings are initialized. Use OTA for routine updates.

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
| `backup`, `flash`, `provision`, `monitor` | USB installation, identity and recovery |
| `status`, `upload`, `rollback`, `diagnostics` | Local management and confirmed deployment |
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
- [Recovery and signing-key backups](docs/recovery.md)
- [Release workflow](docs/releases.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Hardware validation and remaining gates](docs/validation.md)
