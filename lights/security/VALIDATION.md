# Security package validation record

Local validation completed **2026-11-16**, on Apple Silicon macOS 26.5.2 with Apple Swift 6.3.3 and Command Line Tools. SwiftProtobuf 1.39.0 is locked to commit `3d537cb81aef272a03cb3be815439e9bf0eb3bd3`.

## Passed

`bash lights/security/scripts/check.sh` builds an optimized native executable and completed **17 check groups, 351 assertions, 0 failures**. The build emitted no warnings. The ignored local log is `.research/release-checks.log`.

- Complete SRP handshake/protobuf/AES-GCM interoperability against three reproducible fixtures: normal, leading-zero client public key, and short server public key.
- Pinned ESP-IDF Python client cross-check of session keys for all three; proof comparison for the two cases where upstream Python's minimal A encoding matches the native server's required transmitted A representation. The leading-zero-A case uses the independent native-server formula with the required 384-byte A.
- Independent reproduction of the salt/verifier pair embedded in pinned IDF `test_srp.c`.
- Native verifier generation matches that IDF pair and an independent fixture with a leading-zero salt and leading-zero inner SHA-512 digest. The full 64 digest bytes are retained; integer serialization must not strip the leading zero before the outer hash. Random verifier salts are 16 bytes and verifiers are left padded to 384 bytes.
- Cross-language Python `cryptography` → Apple CryptoKit P-256/SHA-256 binding signature and exact UTF-8 signature context.
- Exact four-record transcript hash and shared AES-GCM counter sequence, including encrypted binding at counters 1/2 and subsequent application exchanges at 3/4/5/6.
- Rejection of legacy patch versions, invalid credential bounds, malformed/truncated/oversized handshakes, wrong scheme/message/payload/status, invalid B values, bad salt sizes, duplicate/unknown protobuf fields and noncanonical encodings.
- Rejection of wrong password, tampered/short server proof, invalid nonce length, and initial counter 0.
- Application requests blocked until the trusted gateway identity check succeeds.
- Rejection of changed gateway ID, boot, transcript, challenge, role, signing key, signature or operation.
- Strict flat binding JSON rejects duplicate keys, escaped key aliases, embedded-NUL aliases, wrong types, nested values, leading-zero numeric encodings and trailing objects.
- Ciphertext replay, truncation, tampering, oversize, invalid call ordering, concurrent requests and resuming a closed session all fail closed; the authenticated role is cleared.
- Plaintext limit 1,008 bytes, encrypted record limit 1,024 bytes, and nonce increment overflow/zero rejection.

Fixtures: `Tests/LightsSecurityTests/Fixtures/idf61.json`. Generator: `scripts/generate_vectors.py`. Fixed test passwords, AES keys, ephemeral scalars and P-256 signing scalar are public test material. They are never production credentials.

The checked IDF source directories had no uncommitted changes during the audit. Generator revision checking, exact dependency pins and preserved source/license metadata make the offline checks reproducible. Python test versions were `cryptography==46.0.5` and `protobuf==6.33.2`.

## Native application adapter checks

On the same date, `bash lights/security/scripts/check-gateway-wire.sh` verified the
Python dependencies and IDF pin, built the optimized macOS runner, and completed
**52 encrypted adapter assertions with zero failures**. The ordinary native UI
and transport checks in that build also completed **95 assertions**; subsequent
UI-only additions have their own native validation record.

The adapter fixture exercises real native SRP, P-256 transcript verification and
AES-GCM against a local Python peer through bounded process pipes. It covers:

- Default disabled gate; exact gateway pages; no remembered-secret writes without consent.
- Fresh revisions before the final room lease, canonical UUID/sequence/expiry fields, and eight revision positions.
- Matching receipts plus current readback, stale UI rejection, unknown observations, superseded settings, pending results, and later resolution by the original UUID without another write.
- Lost-reply recovery to a new authenticated path with the same grant and at most one active channel; expected idle renewal on the available route.
- Revoked grants never automatically reenroll; explicit rejoin; visible Keychain deletion failure and successful retry.
- Owner capability boundaries, occupied session pagination, freeze state without invented expiry, verifier-only password rotation, encrypted home-key import, discovery and explicit adoption.
- Storage maintenance and typed signing setup blocks; malformed duplicate JSON closes the channel.
- BLE busy and authentication refusals remain distinct and do not trigger a second route probe.

The fixture is a scripted application peer, **not the C service or physical
hardware**. It makes no relay/BLE socket connection, Wi-Fi change, Keychain write,
ESP32 call or WiZ command. The first harness attempt was stopped after a blocking
Foundation pipe read; the fixture now uses one bounded POSIX read after `poll`,
and the complete optimized run above passed. See [GATEWAY_ADAPTER.md](GATEWAY_ADAPTER.md)
for the one-owner-slot recovery limit and deployment boundaries.

## Not claimed

- No firmware was flashed, device configuration changed, Wi-Fi connection altered, or production credential accessed by this package work.
- No real ESP-IDF server session, BLE connection, relay session, WiZ bulb control, memory/coexistence test or soak test was performed here. Native C formulas and wire behavior were reviewed; executing a live C endpoint is a separate integration gate.
- No external cryptographic audit. The new P-256 transcript-binding extension requires review in addition to the upstream Security2 protocol. The server must enforce the same binding gate and compute the transcript locally.
- No guarantee of constant-time BigUInt arithmetic or complete zeroization of Swift/CryptoKit copies.
- No owner-key distribution, trusted-descriptor enrollment, Keychain storage, password-strength policy, server authentication throttling, revocation policy, relay authorization or code-signing/notarization implementation in this package.

## Tooling observations

The original source compiled successfully in debug mode. Full XCTest is absent from this Command Line Tools installation; the checks therefore use a standalone executable, not a fake XCTest adapter. An initial unoptimized run was stopped to avoid the large performance cost of upstream BigUInt under debug compilation; the complete optimized suite passed. This does not mask a failed assertion.

The global Homebrew `protoc` binary could not load its matching abseil dylib. No global tool was modified. Protobuf sources were instead regenerated with the pinned Apple plugin via the standard `CodeGeneratorRequest` protocol using the pinned IDF descriptors. The generation step and all runtime cryptographic checks succeeded.
