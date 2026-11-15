# Local validation record — 2026-11-16

Scope: isolated native WiZ protocol/crypto, request tracker, and ESP-IDF adapter.
No physical bulb was discovered, paired, controlled, reset, or removed. No
production firmware, Wi-Fi configuration, cloud service, or display was changed.

| Check | Observed result |
| --- | --- |
| Strict C11 host compile | Passed with AppleClang 21, `-Wall -Wextra -Werror -Wshadow`; repository cJSON linked |
| Actual PSA HMAC-SHA256 | Exact upstream signed-request vector passed against the pinned ESP-IDF crypto source |
| Host checks with ASan/UBSan | **10,838 assertions passed**, including 10,000 deterministic arbitrary input datagrams; no reported sanitizer error |
| Public upstream fixture replay | **52/52 literal system/model/user replies accepted**; two exception-class fixtures excluded without execution |
| Sanitized room-bulb model | Complete ESP25_SHRGB_01 / 1.38.0 model response accepted without parser changes; 2200–6500 K and minimum brightness 10 verified in capability tests |
| ESP32-C3 compile/link | WiZ and integrated gateway harnesses passed with ESP-IDF v6.1 and GCC 15.2.0; real Wi-Fi, netif DHCP, monotonic timer, lwIP and PSA APIs |
| Static storage | `sizeof(wiz_driver_t)` = 3928 bytes on host, **3896 bytes on ESP32-C3**; ESP adapter = **16 bytes on ESP32-C3** |
| Measured RAM reduction | Driver 5944→3896, adapter 2068→16 on ESP32-C3: **4100 bytes reclaimed** without changing accepted packet limits or slot count |
| Service deadline regression | Sanitized gateway suite passed with delayed-send expiry before write and before readback, successful delayed send/readback, truthful SENT tracking and fresh monotonic completion timestamps |
| Source/build isolation | `.build`, `.idf-build`, generated SDK config/lock/cache files are ignored; no build output belongs in production source fingerprint |

Important negative cases exercised: malformed and duplicate decoded JSON keys;
every truncation of a valid state reply; invalid MACs/home keys; missing or lost
signing clock; strict reported CCT/dimming and exclusive styles; unknown-model
capability restrictions; off/style conflict; nonmatching source IP/port/method/MAC;
oversized datagrams and excessive nesting; ACK without observation; stale state
during a fade; three-read final mismatch; cancellation before retry; DHCP drift;
no associated peer; authentication rejection without unsigned retry; total deadline
and monotonic rollback; fixed per-poll receive work; enrollment aliases and removal.

The final-send guard samples monotonic time after association/DHCP work, checks
the original deadline and current authority immediately before the nonblocking
`sendto` syscall, and propagates its fresh sample back to the service. Tests model
one/two-millisecond preparation delays crossing expiry; no write or readback is
sent after the failed fence. A write already handed to the socket remains marked
sent even if later bookkeeping fails. This is a syscall-boundary guarantee, not
a claim about RF delivery timing or atomicity with an independently running
policy writer; the worker and authority must remain serialized.

The one shared driver packet buffer and direct adapter receive were checked with
exact 2048-byte valid responses, 2049-byte responses with valid JSON prefixes,
oversized packets reported by the adapter and defensively detected from length,
foreign oversized packets, signed retries after receive-buffer overwrite, and
readback after acknowledgement. The extra byte is detection space, not a larger
accepted payload. The target sizes above were read from the compile-check object
using `riscv32-esp-elf-nm -S` (`driver.0 = 0x0f38`, `adapter.2 = 0x10`).

The fixture replay initially rejected two real 1.38.0 model replies (499 and
577 bytes) because the first structural-token cap was unnecessarily tight. The
final cap is 192 tokens, above their maximum of 153, while maintaining a 2048-byte
datagram limit and six-container nesting limit. No model-specific parser bypass
was added. Tests now cover firmware changes invalidating prior capability evidence.

The ESP target is a **compile/link-only harness**, not the final product and not a
capacity measurement of the complete display/gateway firmware. It deliberately
calls only invalid no-I/O initialization paths and was not flashed. Physical
compatibility, onboarding, frame-time coexistence, stack watermark, free-heap
stress, packet timing, and actual response-signature support remain unverified.

Subsequent read-only hub evidence supplied the reviewed model-only fixture in
tests/fixtures. Its source and limits are recorded there. This adds actual model
parsing evidence; it does not establish signing, write compatibility, gradient
support, scene fidelity, or complete light control.

Use the commands in [README.md](README.md) to reproduce. The upstream fixture
loader requires checkout `19e607afe0eb6db1473602639e194d691234d4fd` and refuses
another revision. The host sanitizers instrument this component and cJSON; the
actual linked cryptographic dependency is tested by known answers, rather than
being described as exhaustively audited or fully sanitizer-instrumented.
