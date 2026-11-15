# Sources and implementation boundary

This is a narrow browser port of the project's pinned Espressif Security2
profile. It is not an official Espressif JavaScript SDK or an externally audited
cryptographic library. Source hashes and exact revisions are in `UPSTREAM.json`.

- [ESP-IDF v6.1 protocomm](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/provisioning/protocomm.html)
  documents Security2 as SRP6a plus AES-GCM with protobuf session establishment.
- [ESP-IDF source at fff9895](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643/components/protocomm)
  supplies the pinned server, protobuf definitions, Python client reference and
  native salt/verifier test. The existing `lights/security` fixtures independently
  cross-check those implementations and are replayed here.
- [Espressif provisioning iOS 3.0.3 at ff70c09](https://github.com/espressif/esp-idf-provisioning-ios/tree/ff70c09ded605969e053ade44a6b8756b93c4e4c/ESPProvision/Security2)
  is the upstream basis of `lights/security/Sources/EspressifSRP`. Its SRP
  calculations and 3072-bit group are adapted here. The SRP portions retain
  Bouke Haarsma's MIT notice; Espressif portions use Apache-2.0. Copies are in
  `licenses/`. No BigUInt source is included; JavaScript's native BigInt is used.
- [Web Cryptography](https://www.w3.org/TR/webcrypto/)
  defines the browser hashing, AES-GCM, SPKI import and ECDSA verification APIs.
  The protocol's DER ECDSA signatures are strictly decoded into the 64-byte
  `r || s` representation expected by WebCrypto. No external crypto CDN,
  runtime script download, dynamically evaluated code, or WASM binary is used.

The primary-source review did not establish an official browser Security2 SDK
to reuse. A generic SRP library would not prove compatibility with IDF's exact
padding rules. This implementation therefore exposes only the protocol profile
checked against those fixtures: SHA-512, RFC 5054 group 3072/g=5,
32-byte CSPRNG ephemeral, padded 384-byte
A, padded A/B for `u`, **received B bytes** in M, **minimal S bytes** in K, and the
first 32 bytes of K as the AES-256 key. It rejects invalid public B and zero S.
These are interoperability and negative checks, not an independent security audit.

The transcript-bound P-256 identity proof is this project's application extension
from `lights/security/IDENTITY.md`, not an upstream Espressif feature. It still
needs independent protocol review and end-to-end hardware qualification.

Native JS BigInt arithmetic and the JavaScript proof comparison are **variable
time**, and are not hardened against same-process timing analysis. WebCrypto
does the hash/AES/ECDSA operations, but provides no SRP primitive. Buffers are
cleared where practical and AES CryptoKeys are non-extractable; garbage
collection, immutable strings, BigInt values, engine copies and the caller's
input field prevent a guarantee that all historical secret copies are erased.
The site must not load third-party scripts, analytics or extensions into this
credential flow. A compromised same-origin page can access entered credentials;
channel encryption does not protect against malicious application JavaScript.
