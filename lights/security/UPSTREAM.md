# Source and dependency audit

| Source | Pinned revision | License | Used for |
|---|---|---|---|
| [Espressif provisioning-ios 3.0.3](https://github.com/espressif/esp-idf-provisioning-ios/tree/ff70c09ded605969e053ade44a6b8756b93c4e4c) | `ff70c09ded605969e053ade44a6b8756b93c4e4c` | Apache-2.0; embedded MIT files | SRP, BigUInt, patched nonce reference |
| [ESP-IDF v6.1](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643) | `fff9895c82d744c7237be8847347bdd1b07c6643` | Apache-2.0; example test CC0/Unlicense | Wire schemas, server/Python reference |
| [Apple SwiftProtobuf 1.39.0](https://github.com/apple/swift-protobuf/tree/3d537cb81aef272a03cb3be815439e9bf0eb3bd3) | `3d537cb81aef272a03cb3be815439e9bf0eb3bd3` | Apache-2.0 | Protobuf runtime/generator |
| Apple CryptoKit/Security/Darwin | macOS system SDK | Apple platform frameworks | SHA-512, SHA-256, AES-GCM, P-256, CSPRNG, constant-time comparison, buffer clearing |

The exact SwiftProtobuf version is in `Package.swift` and revision in `Package.resolved`. `UPSTREAM.json` records per-file source hashes. License texts are retained in `Licenses/`; source copyright notices remain intact.

Espressif's iOS package includes UIKit/CoreBluetooth/provisioning flows and broad dependency ranges. Only the independent mathematical source was retained. No Wi-Fi scanning, password logging, iOS UI, GATT code or Security0/1 implementation is linked. Generated protobuf declarations still describe the upstream Security0/1 fields so their wire messages can be parsed and explicitly rejected; there is no fallback implementation.

## Deliberate source changes

- BigUInt files are unmodified except the two `fileprivate (set)` whitespace corrections required by modern Swift diagnostics. `Group.swift` is unmodified. Both remain under their original MIT license.
- `SRPClient.swift` adapts Espressif's MIT `Client.swift` and SRP formulas. It restricts the group/hash, uses checked `SecRandomCopyBytes`, supports a package-private deterministic scalar only for fixtures, bounds all inputs, rejects `B <= 0`, `B >= N`, and `u == 0`, and does not expose the key until proof verification. It uses Darwin `timingsafe_bcmp` for the 64-byte proof comparison. The separate bounded verifier helper uses the same group and SHA-512 formula, retaining all inner digest bytes to match the IDF native server even when the digest starts with zero.
- Client public key A is left padded to exactly 384 bytes because the pinned IDF Security2 server requires that length. Proofs hash that transmitted representation. Server key B and shared secret S retain the native server's minimal unsigned serialization. This fixes the iOS client's intermittent leading-zero-A incompatibility without changing the SRP mathematics.
- Passwords use byte buffers rather than interpolated Strings. The wrapper releases mutable credentials and transient SRP state early, with documented best-effort clearing limitations.
- The iOS `ESPSecurity2` wrapper is not copied. It permits the old nonce-reuse mode, lacks several parse/status/size checks, uses force unwraps and has an unchecked counter increment. The new actor wraps the same corrected patch-1 wire behavior with fail-closed state, nonce exhaustion checks and one in-flight request.
- The pinned iOS patch constructs the first nonce counter as 1. The native and Python implementations confirm the eight-byte prefix plus big-endian 32-bit counter and one shared ordered sequence. This package requires the server's nonce to encode counter 1 and never offers patch 0.
- Protobuf sources are regenerated from unchanged pinned IDF descriptors with Apple's pinned generator, package-level visibility, and computed `json_name` metadata. The local Homebrew protoc binary had a missing abseil dylib; no global tools were repaired. `scripts/regenerate_proto.py` uses the standard `CodeGeneratorRequest` interface directly instead. Included schemas show the unchanged wire fields.
- P-256 channel binding is an application extension documented separately, not a modification of SRP/AES-GCM or a feature attributed to upstream.

## Known upstream/security limitations

BigUInt modular exponentiation is variable-time and Swift does not guarantee total secret zeroization. The original iOS source is maintained by Espressif, but that is not an independent security audit of this adaptation. The native IDF debug logging includes secrets. Its Security2 handle supports one active session per instance; independent clients require independent server handles and a bounded admission policy. These are integration constraints, not issues hidden by the client package.

Primary implementation references: [iOS Security2](https://github.com/espressif/esp-idf-provisioning-ios/blob/ff70c09ded605969e053ade44a6b8756b93c4e4c/ESPProvision/Security2/ESPSecurity2.swift), [iOS SRP client](https://github.com/espressif/esp-idf-provisioning-ios/blob/ff70c09ded605969e053ade44a6b8756b93c4e4c/ESPProvision/Security2/SRP/Client.swift), [IDF native Security2](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/protocomm/src/security/security2.c), [IDF Python Security2](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/examples/protocols/esp_local_ctrl/scripts/security/security2.py), [official protocomm documentation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/provisioning/protocomm.html).
