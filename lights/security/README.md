# LightsSecurity

Native macOS, transport-independent client for **ESP-IDF v6.1 protocomm Security2, patch 1 only**. It uses Espressif's SRP-6a/SHA-512/3072-bit implementation and Apple's CryptoKit AES-256-GCM. No Security0/1 fallback, networking, UI, Keychain access, BLE permissions, or Wi-Fi changes are included.

This package also implements the explicitly separate [gateway identity binding extension](IDENTITY.md). A shared guest password alone cannot distinguish the real gateway from another guest. Application commands stay blocked until the gateway proves possession of the independently trusted P-256 device key.

The package compiles and has offline cross-language checks. This is **not evidence of a live BLE/relay session, firmware resource suitability, physical WiZ control, or an external cryptographic audit**. See [VALIDATION.md](VALIDATION.md) for the exact tested boundary and [UPSTREAM.md](UPSTREAM.md) for source changes and limitations.

## Integration

Add `.package(path: "../security")`, then `.product(name: "LightsSecurity", package: "security")`. The minimum deployment target is macOS 14; the package manifest requires Swift 6.1. The vendor's original BigUInt source is compiled in Swift 5 compatibility mode.

```swift
import LightsSecurity

let session = try Security2Session(
    username: "203-guest", // or "203-owner", selecting its distinct verifier
    password: credentialBytesFromKeychain,
    securityPatchVersion: 1
)
let command0 = try await session.startHandshake()
let response0 = try await transport.exchange(op: "session", bytes: command0)
let command1 = try await session.receiveChallenge(response0)
let response1 = try await transport.exchange(op: "session", bytes: command1)
try await session.verifyServer(response1)

let binding = try await session.makeIdentityBindingRequest(
    trustedGateway: ownerVerifiedDescriptor,
    expectedBoot: currentBootFromConnection,
    expectedRole: .guest
)
let bindingReply = try await transport.exchange(op: "data", bytes: binding)
try await session.verifyIdentityBindingResponse(bindingReply)

let request = try await session.sealRequest(encodedApplicationRequest)
let response = try await transport.exchange(op: "data", bytes: request)
let plaintext = try await session.openResponse(response)
```

`transport` above is the integrating app's serialized byte transport, not a class supplied by this package. It must validate its connection identifier, request identifier, boot identifier, operation, status and size **before** delivering a response. Public relay framing is specified separately in `relay/LIGHTS_PROTOCOL.md`.

On any timeout, cancellation, transport failure, unexpected reply, or route change, call `await session.close()`, close the underlying logical connection, discard queued ciphertext, and create a fresh session. Never retry a ciphertext or move an existing session between BLE and relay. Request application status after a fresh connection to determine whether a timed-out operation executed; do not assume it failed. Application operation IDs and deduplication belong to the policy/transport layers.

## Protocol limits

| Item | Limit |
|---|---:|
| Handshake record | 1,024 bytes, protobuf nesting depth 4 |
| Username | 1–64 UTF-8 bytes; no NUL |
| Credential | 1–256 raw bytes; credential strength enforced by enrollment policy |
| Client public key | exactly 384 bytes, left padded |
| Server public key | 1–384 bytes; value strictly between 0 and group N |
| Salt | 16–64 bytes |
| SRP proof | exactly 64 bytes |
| Nonce | 12 bytes, initial counter exactly 1 |
| Encrypted record | 1,024 bytes, including 16-byte tag |
| Plaintext | 1,008 bytes |
| In-flight requests | exactly one |

Protobuf handshakes require the expected Security2 message, successful status, correct oneof payload, no unknown fields, and canonical encoding matching the pinned implementations. Malformed, oversized, out-of-order, replayed or unauthenticated input closes the session. Errors contain only stable classifications and no peer content or secrets.

**Counter contract:** IDF uses one shared sequence, not separate receive/transmit counters. The first client bind request uses nonce counter 1, its server response 2, the next client application request 3, and response 4. The 8-byte prefix is per-session. The counter never wraps; the client reserves room for a response and rejects exhaustion. The integration must periodically reauthenticate, rate limit authentication, enforce lifetime/idle deadlines, and serialize even requests from different UI controls. Unsolicited encrypted server pushes are not supported by this protocol profile.

## Local checks

From the repository root:

```sh
bash lights/security/scripts/check.sh
```

This resolves the exact SwiftProtobuf pin if necessary, builds an optimized command-line runner, and exercises real CryptoKit/SRP operations using fixed **test-only** fixtures. It works with Apple's Command Line Tools without full Xcode. No device or network connection is made by the runner. SwiftPM may access GitHub only to resolve the dependency. `.build`, `.research` and SwiftPM state are ignored.

Offline fixture regeneration uses Python `cryptography==46.0.5`, `protobuf==6.33.2`, and the pinned IDF checkout. All stored passwords, scalar values and keys in fixtures are intentionally public test material, never production credentials. `scripts/generate_vectors.py` checks the IDF revision before running. No global Python or Homebrew installation is required or altered.

Create the isolated test environment with `bash lights/security/scripts/bootstrap-test-env.sh`. It installs pinned test packages under ignored `.research/venv`, without changing the system Python. To use an existing compatible environment, set `LIGHTS_TEST_PYTHON` to its absolute Python executable path. The native adapter's optional `--gateway-wire-checks` runner uses that override, otherwise this isolated environment; it communicates with a scripted peer through local pipes only.

Run `bash lights/security/scripts/check-gateway-wire.sh` for the native application adapter's encrypted local-peer checks. The wrapper also verifies the IDF revision and Python package versions and builds the optimized native runner. [GATEWAY_ADAPTER.md](GATEWAY_ADAPTER.md) describes the integration, operation reconciliation, saved-access behavior, and recovery limits.

`SRPVerifier.generate(username:password:)` creates a 16-byte random salt and a 384-byte verifier for owner-authorized credential rotation. Invoke it away from the main actor because modular exponentiation is CPU intensive. The salt and verifier are sent inside the already authenticated owner channel; a verifier is not a substitute for a strong guest password. The helper uses full SHA-512 digest bytes, including leading zero bytes, as required by the native server.

## Credential handling

Load production credentials from Keychain immediately before constructing a session. Do not interpolate them into logs, URLs, errors, analytics, crash attachments or relay metadata. Release caller-owned copies once no longer needed. The library has no logging calls. It clears mutable password buffers with `memset_s`, releases the SRP scalar and temporary proof/key state after authentication, and removes the AES key when closed.

**Swift value copies, BigUInt temporaries and CryptoKit internals prevent a guarantee that every historical memory copy is zeroized.** Upstream BigUInt is variable-time; this package is not hardened against a local process/memory/timing adversary. A compromised Mac remains outside its protection boundary. These limitations require review before claiming a high-assurance cryptographic implementation.

On ESP-IDF, `security2.c` and `esp_srp.c` contain DEBUG logging of sensitive material. Firmware must keep those component log levels above DEBUG and prevent runtime log-level escalation. Merely avoiding logs in the native app is insufficient.

Role authorization, guest password rotation, owner precedence, revocation, session caps, replay-safe application operation IDs and trusted-descriptor enrollment are separate responsibilities. Successful SRP or device binding does not itself authorize an arbitrary light operation.
