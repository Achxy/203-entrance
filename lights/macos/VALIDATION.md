# Native candidate validation — 16 November 2026

This record covers the macOS candidate and host fixtures. It does not certify the installed ESP32, real bulbs, independent provisioning, or a distributable release.

| Check | Evidence | Boundary |
| --- | --- | --- |
| Optimized native model/protocol checks | `bash lights/macos/scripts/check.sh`: **100 assertions passed** | No network sockets, Bluetooth scans, real Keychain writes or light commands |
| Encrypted gateway application checks | Security agent's `check-gateway-wire.sh`: **52 assertions passed** | Actual Swift SRP, P256 identity binding and AES against a local scripted peer; no ESP32 or bulbs |
| Native layouts | Ten menu-panel PNGs plus four owner verification/setup detail PNGs generated and visually inspected in light/dark | Offscreen views; owner sidebar does not render; no interactive keyboard/VoiceOver proof |
| Local app bundles | Release arm64 and universal arm64+x86_64 bundles; exact architecture set, macOS14 deployment records, plist and all-slice strict code signatures verified | Ad-hoc signatures; Intel execution, Developer ID and notarization unverified |
| Development ZIPs | Both modes passed ZIP integrity, extraction, all-slice signature verification, exact regular-file inventory and every-file SHA-256 checks | No package was installed, launched, uploaded or presented as a general-distribution release |
| Launch behavior | Default constructs the real adapter but remains disconnected; preview requires `--preview`; construction of transport/discovery seams is inert | No real application launch or permission dialog was exercised by this native implementation pass |

The ordinary suite's last five assertions were added after the encrypted run: explicit Saved Access, entered-password rejoin, and foreground refresh lifecycle. The later probe adds a read-only authenticated-boot getter; it does not alter the encrypted application protocol. The final packaging pass changes no app, adapter or security source.

Host: macOS 26.5.2, Apple Silicon, Swift 6.3.3 Command Line Tools. The deployment target is macOS 14; execution on macOS 14 and Intel remains unverified. No valid signing identities were available during inspection.

Current development executables:

```text
lights/macos/build/203 Lights.app
Executable SHA-256:
af09276ab98732d61662515f7d0b8b26e82f88a8ba1be359a995707d6f7767cc

lights/macos/build/universal/203 Lights.app
Executable SHA-256:
793f6513611a9859253c6a93eb5c56c9c2202ae0695bd9e64c5b32dc183bdc97
```

Generated evidence is ignored local output: `lights/macos/check-results.log`, `package-results.log`, `package-universal-results.log`, `snapshot-results.log`, each package's `BUILD-MANIFEST.json`, and `.local/lights-ui/manifest.json` plus its fourteen PNGs. The failed early ImageRenderer attempt omitted native controls and is not used as evidence. Actual CUA/native attachment was unavailable, so interactive menu dismissal, real-window focus notifications, color-panel focus, permission prompts, keyboard navigation and VoiceOver remain open checks.

The owning agent subsequently launched the development bundle and verified its
process, but native CUA attachment again timed out. It then closed only that
owned, disconnected process and rebuilt the bundle with the final owner notices.
The updated bundle passed plist and strict ad-hoc signature verification; the
hashes above reflect the subsequent packaging pass below. A running process is not an interactive UI
validation result. No app is left running by that check.

No source in this pass switched the Mac's Wi-Fi, changed routes, accessed the private WiZ export, imported a real key, flashed production firmware, or operated a light. Official WiZ pairing and local export are labeled only as a development bootstrap. Standalone commissioning and its key establishment remain a separate requirement; daily controls do not depend on WiZ cloud accounts.

Release gates also include physical verified-control/readback, owner/guest races across real Internet and BLE, retry/resource/latency behavior with the display running, and signed-app Keychain behavior. See [README.md](README.md) for the implementation boundary and distribution requirements.

## Owner access-boundary clarification

The owner chose to leave WiZ on **Allow all controls**. The source now explicitly says that the local key signs outgoing gateway commands, LAN replies remain unverified, and importing the key does not prove rejection of unsigned outsiders. A persistent owner Local Verification notice explains that pauses and revocations cover this app/site while other clients on the bulb Wi-Fi may bypass them. Unsigned fallback remains off; no WiZ setting was changed.

After this wording change, the optimized native suite again passed **100 assertions**, and the snapshot runner generated **14 images**. The four added `owner-verification-{light,dark}.png` and `owner-setup-{light,dark}.png` were inspected for the changed detail content. Their native navigation sidebar is blank in this offscreen environment, so they are not a complete owner-window visual pass. Interactive behavior remains unverified.

The UI agent left the running bundle unchanged during that follow-up. The owning
agent subsequently closed it and completed the rebuild described above. The
recorded executable hashes include this copy correction.

## Universal packaging

The installed Swift 6.3.3 Command Line Tools and macOS 26.5 SDK successfully cross-compiled both `arm64-apple-macosx14.0` and `x86_64-apple-macosx14.0`. The default `build-app.sh release` remains arm64; `build-app.sh release --universal` combines both slices and uses a separate output directory. The initial architecture-validation invocation had an incorrect `lipo` argument order; that was corrected before either verified ZIP was published locally.

| Archive | Bytes | SHA-256 |
| --- | ---: | --- |
| `build/203-Lights-arm64-development.zip` | 1,853,067 | `e89889cddeaf3e417ad053b851984a7c599d313aaccab41596e84932e498e30e` |
| `build/universal/203-Lights-universal-development.zip` | 3,801,545 | `b01e02c564e48767b03a30adc098714a95fb2eb9dda31c528fc18a06c1297c59` |

Each archive contains a development notice, architecture/toolchain/hash manifest, complete file hashes, the app's privacy resource and all referenced third-party license/notice files. The corresponding `.zip.sha256` records the whole-archive hash. Before replacing an output, the script checks that its generated executable is not running; its previous bundle is preserved. Cached dependencies must match the clean pinned revision, and SwiftPM update/prefetch and credential lookup are disabled.

The universal artifact demonstrates build and package compatibility, **not Intel runtime validation or public-release readiness**. It is ad-hoc signed, has no Developer ID identity or notarization, and still needs the existing hardware, GUI, signing and Gatekeeper release gates. No Rosetta install, account operation, new dependency fetch, network connection, UI launch or production build change was performed for this packaging task.
