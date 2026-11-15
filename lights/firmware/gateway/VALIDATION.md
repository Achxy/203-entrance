# Gateway component validation

Recorded 16 November 2026. Candidate source only; no production installation or
bulb/network mutation was performed by this component's work.

| Check | Result and boundary |
|---|---|
| Host compile | C11, `-Wall -Wextra -Wconversion -Werror`; ASan and UBSan enabled. |
| Persistent record | 3072-byte architecture-independent record; round-trip checked; all 24,576 one-bit corruptions rejected. |
| Crash boundaries | Mock storage tests blob/head commit separation, orphan newer blobs, corrupt selected record, ambiguous failed head acknowledgement, and no older-policy fallback. No actual power interruption. |
| Application integration | Real authority/parser/driver code with simulated UDP checks admission, resume, duplicates, queued/readback states, durable policy and grant changes, read-only discovery and adoption. No claim of physical bulb response. |
| Owner precedence | A silent guest target is preempted; an owner command to another target sends within the next two simulated ticks, without waiting for the guest deadline. |
| Revocation and renewal | Unrevoked remembered token renews in the same slot under a new ID after boot; revoked token and corrupt state fail closed; an old-ID revoke returns conflict. |
| Resource limits | Five enrolled bulbs; eight stable slots; three application route bindings with 60-second orphan expiry; 16 grant records; bounded JSON depth and preparse structural-token count. |
| Output honesty | Missing trusted signing clock produces a rejected command with no UDP send; a configured home key does not clear legacy-control risk. |
| Native ESP32-C3 compile/link | Passed with the repository's pinned ESP-IDF 6.1 toolchain in a separate compile-only project. Includes real NVS and WiZ adapter APIs. Never flashed. |
| Combined hardware stability | Not tested by this component; must include display/games, five bulbs, BLE/Internet, TLS reconnect, storage, signed OTA and a 60-minute soak. |

The host service object currently measures 18,216 bytes, including authority,
driver, record, route bindings and discovery candidates. The ESP network adapter,
worker stack, crypto sessions and temporary NVS/JSON allocations are additional.
This number is not available free heap and is not a claim that the production
C3 has sufficient capacity.

Private local build logs are retained in `.local/lights-service-check/`; host
artifacts in `.local/lights-gateway-checks/`; the target compile fixture in
`.local/lights-gateway-idf/`. Run the commands in README to reproduce rather than
shipping those local artifacts or treating fixture firmware as a release.
