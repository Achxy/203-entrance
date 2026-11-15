# Browser protocol validation — 2026-11-16

This record covers the browser Security2 implementation and bounded relay pipe.
It does not claim a successful physical bulb write, Safari/iPhone execution,
production deployment, DNS change, or independent cryptographic audit.

| Check | Observed result |
| --- | --- |
| Node v26.10.0 native WebCrypto harness | **16 tests passed**, zero failures; no remote network or device I/O |
| ESP-IDF interoperability | All three pinned vectors matched exact outgoing protobuf, client proof, server proof, transcript, P-256 binding and AES-GCM request/response bytes |
| Edge-case vectors | Padded leading-zero A and received 383-byte B matched native-reference fixtures |
| Verifier generator | Exact ESP-IDF `test_srp.c` salt/verifier matched; fresh guest salt/verifier independently recomputed |
| Source pins | Nine local reference hashes and the copied public fixture corpus verified; pinned ESP-IDF commit checked |
| Actual browser run | Root agent ran the included harness through CUA in **IAB Chrome 157**: all three vectors and **33 browser assertions passed**; browser error/warning logs were empty |
| Browser coverage boundary | Safari, actual iPhone, actual Android phone, private-browsing storage behavior, and a live gateway handshake were not tested by this component's checks |

The Node suite also exercises duplicate and escaped JSON keys, unsafe/fractional
numeric encodings, invalid UTF-8, prototype keys, limits, every truncation of a
real protobuf response, noncanonical varints, unknown/duplicate protobuf fields,
wrong security scheme, invalid B/proof, signature/boot/role/key mismatch,
application access before binding, ciphertext mutation/replay, concurrent calls,
transport identity/ID mismatch, malformed base64, timeout, failed WebSocket
construction/send, authenticated application rejection without retry, and close
during request pacing.

The simulated complete channel checks that no grant is enrolled automatically,
no password appears in a relay frame, role is exposed only after signed binding,
all physical sends (including SRP) are at least 450 ms apart, and cancellation
while pacing sends no application ciphertext. This simulated pipe is separate
evidence from the actual Chrome cryptographic fixture run.

The browser fixture page contains only public test credentials, deterministic
test random scalars and public test gateway keys. Product builds must omit the
tests/fixture directory. The module itself performs no persistence or telemetry.

Repeat using the commands and browser harness in [README.md](README.md). The
variable-time SRP arithmetic, best-effort memory clearing, owner-verified trust
bootstrap, and new identity-binding extension are documented in
[NOTICE.md](NOTICE.md). Passing these checks is not a substitute for independent
protocol review or hardware qualification.
