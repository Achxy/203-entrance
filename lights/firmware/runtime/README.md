# Lighting runtime coordinator

This component is an **isolated candidate**. It does not modify production
startup, network configuration, display code, partition tables or the relay on
its own. The parent firmware must explicitly include and initialize it behind
the candidate feature flag. No hardware qualification is implied by a build.

## Lifecycle

1. After the existing storage initialization, call
   `lighting_runtime_prepare(config)`. It opens the existing `state/lights_v1`
   namespace read-only and checks the committed head/record length. Missing
   configuration stays inactive; an existing head with a missing record fails
   closed. There is no public first-owner claim or automatic erase.
2. After the existing AP starts, call `lighting_runtime_start()`. An
   unprovisioned device allocates no service context, creates no worker and
   starts no Bluetooth or UDP work. Configured devices start one idle-priority
   worker; that worker validates and loads the actual record before admission.
3. Only an already authenticated management/USB owner path may call
   `lighting_runtime_provision_trusted_admin(profiles)`. It copies two SRP
   salt/verifier profiles into one bounded pending slot, with a fixed30-second
   deadline and a boot-scoped generation. It does not create a worker in the
   management TLS request. The client closes TLS so the existing relay poll can
   admit the worker after the first one-second quiet interval.
   The function returning success means **queued**, not durable success.
   Return HTTP202 and poll `provisioning` / `provision_result` if exposing this
   through the management API. Existing credentials are not overwritten.
4. The worker alone owns gateway policy, grants, NVS mutations, Security2
   instances and the WiZ adapter. The AP netif callback supplies the existing
   AP interface; no network is created, joined or changed by this component.
5. `lighting_runtime_quiesce(timeout<=7000)` requests a reversible pause and
   waits for acknowledgement. It closes crypto sessions, stops BLE, cancels
   UDP work and closes sockets, while retaining authority state and owner
   holds. Resume waits until the supplied OTA/release pause callback clears.
   No irreversible Bluetooth memory-release API is used.
6. Call `lighting_runtime_poll_recovery(now_ms)` from an existing parent loop,
   including while Internet/relay use is disabled. It does nothing until the
   explicit startup call has occurred. Only configured, storage-healthy,
   inactive instances whose startup failed with `ESP_ERR_NO_MEM` qualify.
   Attempts wait 30, 60, 120, 240, then 300 seconds; later failures remain at
   five-minute intervals. OTA, quiesce, storage faults, a running worker and
   pending provisioning prevent admission. There is no new scheduling task,
   timer, credential replacement, state erase or reboot. Uncommitted pending
   credentials are wiped on failure and require another trusted provision call.

## Deferred trusted provisioning

The existing authenticated management POST still returns202, meaning accepted
into RAM only. It requires healthy storage, no committed lighting record, no
other pending request/worker, no abnormal-reset quarantine or OTA/release pause,
and a recovery poll observed within five seconds. Duplicate or replacement
POSTs return409; they do not replace profiles or extend the deadline. Generation
exhaustion fails closed. A reboot discards uncommitted RAM authorization.

The pending store owns exactly800 profile bytes plus bounded metadata. Once per
second, the existing relay poll may attempt the unchanged107KiB pre-task gate;
the worker still rechecks96KiB before allocating its context. Peak contention,
low heap and fragmentation leave the same request queued until its original
deadline. Once a worker is admitted, a failure before commit ends that request,
wipes the shared profiles and requires a new explicit admin call. This interval is
separate from the30–300-second recovery of an already committed configuration.

`lighting_runtime_housekeeping` must run from the existing health tick as well
as the relay poll. It takes the runtime mutex only with a zero timeout, performs
no allocation/NVS/network operation, and wipes a queued or admitted request at
expiry or cancellation. This is necessary because relay DNS/connect can block.
Wiping occurs at the next scheduled health tick that acquires the short mutex;
the worker independently rejects late admission/commit even before that tick.
The current250ms health cadence is not a real-time guarantee during a stalled
CPU. OTA quiescence also cancels directly and waits for an admitted worker to
finish cleanup; disappearance after an initialization failure counts as stopped.

After checking storage and loading any existing record, the worker checks the
same generation, current monotonic deadline and pause state under the mutex.
Commit admission atomically
copies profiles into its private800-byte workspace and wipes the shared slot.
This reuses the existing input union before the runtime is published or any BLE
work starts; a static assertion prevents growing that union. NVS receives only
that private copy. The worker wipes it on return; expiry and
quiesce never erase a buffer another task is reading. The30-second deadline is
the last admission to the COW transaction, not a claim that a flash operation
can be interrupted or finish by that instant. An already admitted transaction
finishes and reports its real outcome before OTA can proceed.

Status exposes `provision_boot`, `provision_generation`, `provision_deadline_ms`,
`provision_poller_ready` and `provision_phase` (`queued`, `admitted`, `committing`,
`committed`, `failed`, `expired`, `cancelled`, or initial `idle`). A committed
record immediately sets `configured`, even if later runtime/BLE admission fails.
`provision_outcome_unknown` marks a failed storage transaction whose durable
outcome cannot be safely inferred; storage then fails closed, with no automatic
overwrite, erase or retry. Cancellation/expiry alone do not mark storage corrupt.

The access CLI closes TLS after POST acceptance, waits four seconds, and performs
short status reads with closed connections and five-second gaps. It correlates
the same boot/generation, never replays the POST, and preserves its owner bundle.
Read transport errors may be retried within a fixed35-second observation window;
that extra observation time cannot renew the server's30-second authorization.
Lost acceptance, a reboot/replacement, uncertain storage or observation timeout
is reported as unresolved rather than successful provisioning.

## Abnormal-reset containment

Preparation samples ESP-IDF's reset reason once. `POWERON`, `SW` and
`DEEPSLEEP` retain ordinary startup. Every other reason, including unknown or
future reasons, quarantines **lighting only** for that boot. A panic, watchdog
or brownout therefore cannot repeatedly restart lighting after credentials
have been committed. Management, display, games, settings and OTA keep their
existing startup paths. Preparation still inspects lighting storage read-only;
the quarantine starts no lighting worker, Bluetooth, UDP or automatic recovery,
and also rejects initial provisioning. It does not erase or rewrite a record.

Authenticated local management may explicitly call
`lighting_runtime_retry_trusted_admin()` once in that boot. It releases the
quarantine, then attempts normal startup if already configured; otherwise it
only permits a later explicit provision operation. `ESP_OK` means the release
was accepted, **not** that lighting is active. If memory admission fails, the
same unchanged 107 KiB gate and configured-only 30–300 second recovery apply.
Retry is refused with `ESP_ERR_INVALID_STATE` during OTA/release/quiesce, with
unhealthy storage, while another worker/provision is present, or if no unused
quarantine release exists. An admission race rejected before a worker exists
restores the unused release; a subsequent crash starts a new quarantined boot.
Both worker-admission checks honor explicit quiescence, including manual starts.

`GET /api/v1/lights` exposes `quarantined`, `quarantine_retry_used` and the
numeric ESP-IDF `reset_reason`. `POST /api/v1/lights/retry` accepts exactly `{}`
after the existing management administrator, origin and CSRF/bearer checks.
It returns 202 plus status for an accepted release, or 409 for an invalid
state/request. It is not a public/guest or lighting-relay operation. A local
AP connection or the USB-to-Wi-Fi hub is needed while lighting is inactive.

This is a boot-local guard, not a persistent crash ledger or attribution of a
crash to lighting. It deliberately also suspends lighting after unrelated
abnormal resets. A power cycle or intentional software reboot permits normal
startup again; an unreported power collapse may appear as `POWERON`. It cannot
fix an unstable power supply, catch faults before application startup, or make
confirmed firmware roll back automatically. No new task, timer, flash write or
reduced heap threshold is added. Existing OTA trials still pause lighting, so
OTA confirmation alone is not a Bluetooth/coexistence qualification.

The worker stack is 10 KiB because the record decoder alone has more than 3 KiB
of measured stack use before callers. The heap context owns one gateway
service, one AP adapter, one Security2 manager, a 3384-byte relay queue and one
shared input union. It does not duplicate the authority core. Measure stack
watermarks and actual heap peaks before reducing any budget.

`worker_stack_sample_valid` stays false until the lighting worker samples its
own stack. `worker_stack_min_free_bytes` then retains the lowest observed
high-water mark across worker restarts during this boot. Sampling occurs at
most once per second, including activation-failure exits; a failed activation
can therefore leave a valid historical sample while no worker is running.
No other task is scanned, and no saved task handle is dereferenced. ESP-IDF
6.1's `freertos/task.h` documents this API in bytes, and the C3 port uses a
one-byte `StackType_t`; the value is not multiplied by a word size. This is
observability, not a stack-size reduction or proof that every path has run.

ESP32-C3 compiler layout sizes are recorded below. The context and local-time
adapter were refreshed with the pinned toolchain and20-socket candidate
configuration after adding the helper; the other contained sizes retain the
linked candidate's DWARF measurements. These are not live heap measurements;
refresh the final linked report whenever structures change.

| Allocation / contained object | Bytes |
|---|---:|
| `runtime_t` heap context |22,120|
| Gateway service, inside context |16,072|
| Security2 manager headers, inside context |1,040|
| AP UDP adapter, inside context |16|
| Local DNS/SNTP adapter, inside context |504|
| Relay queue, inside context |3,384|
| Shared input buffer, inside context |1,072|
| Worker stack, separate allocation |10,240|
| Context plus worker stack subtotal |32,360|

Do not add the contained rows to the context again. This subtotal excludes
allocator/task metadata, NimBLE/controller pools and stacks, parent relay reply
storage, active SRP/AES contexts, transient P-256/TLS/JSON allocations and NVS
save buffers. Only a combined hardware heap trace establishes the actual peak.

## Required integration callbacks

`lrt_config_t` supplies a gateway ID, the same boot ID used by the existing
relay, the AP netif, existing P-256 identity signing, a trustworthy wall-clock
sample, and the existing relay reply sink. It never receives or exports the
private identity key. Wall-clock failure blocks signed WiZ traffic; there is
no unsigned fallback.

`begin_peak` / `end_peak` are mandatory **nonblocking shared reservations**.
They must coordinate with competing TLS handshakes, OTA/release work and other
large allocations. A heap check alone is not a reservation. The phases cover
activation, BLE startup, SRP and application/storage work. `PROVISION` also
covers bounded application JSON/policy persistence and WiZ polling, so it is
not limited to the initial provisioning operation. A callback must not wait
for or recursively quiesce this same lighting worker.

Provisional fail-closed development gates are:

| Operation | Free internal8-bit heap | Largest block |
|---|---:|---:|
| Before creating the worker |107 KiB|24 KiB|
| Initial runtime allocation |96 KiB|24 KiB|
| BLE start / SRP exchange |64 KiB|24 KiB|
| Application/persistence / UDP polling |40 KiB|24 KiB|
| Post-initialization retained floor |32 KiB|24 KiB|

These are conservative development refusal thresholds, **not measured safe
operating limits**. The production24,000-byte health floor is never lowered.
The pre-worker gate includes its 10 KiB stack and a 1 KiB task allocation
margin. Its peak reservation is released before the new worker may initialize;
the worker then reserves and checks again before allocating the service.
Both activation checks also require a largest free block of at least the
runtime context size plus 1 KiB when that exceeds the table's 24 KiB minimum.
Current hardware may refuse activation or SRP until the combined candidate
demonstrates enough headroom. Linker success cannot establish that proof.

Status always samples current heap/largest block, including before any worker
exists. `failure_stage` and `failure_reason` have public string-name helpers;
their timestamp, observed heap and required thresholds describe the last
refused/failed operation. A zero required threshold means that the failure was
not a numeric heap guard (for example, NVS I/O). Busy peak reservations are
distinct from insufficient free heap and fragmentation. Recovery status reports
the next attempt, consecutive failures and total automatic attempts. A successful
activation resets the backoff but retains failure history for diagnosis.

## Relay and Bluetooth

`lighting_runtime_submit_relay` accepts already parsed fields from the
authenticated parent device WebSocket. It validates boot, original pulse
lease/deadline, payload bounds, operation and increasing request IDs, and
copies into three bounded slots. At most two non-owner requests can be pending;
proof-verified owner work receives reserved capacity and priority. There is
one in-flight request per connection. The shared sliding admission window is
eight exchanges/second, at most six non-owner, across both relay and BLE.
An unproven relay route expires ten seconds after its first admission;
repeated identity requests never extend that absolute window.

Data and close leases are at most2 seconds; handshake/identity leases at most
10 seconds. Time is the gateway's monotonic clock, never a client wall clock.
The app should use a conservative shorter command validity interval because
the relay lease comes from the latest250ms device pulse, not from packet
receipt. Late packets cannot refresh their own lifetime.

Parent WebSocket loss calls `lighting_runtime_relay_disconnected()`, which
closes only relay crypto/routes. BLE, durable grants, freezes and holds remain.
An individual client close uses `lighting_runtime_close_relay(route)`.

BLE uses the matching component's queued events, worker notifications and
indication backpressure. GATT callbacks never execute SRP/JSON/NVS/UDP. A role
is assigned only after a valid SRP client proof, with at most one proven guest
BLE link so the other link remains available for owner admission. Identity
binding is still required before application requests. Initial unproven peer
admission and physical RF jamming cannot be completely prevented by this
application protocol.
BLE peers must also obtain a valid application grant within ten
seconds of connecting. This absolute admission deadline applies during every
RX/response phase, so repeated identity requests cannot pin physical links.

The unencrypted identity operation accepts exactly `{"v":1}` (ordinary JSON
whitespace permitted outside strings) and returns:

```json
{"v":1,"protocol":1,"connection":"32_lowercase_hex","boot":"16_lowercase_hex","gateway_id":"entrance-c7f088","security":2,"patch":1}
```

These fields are discovery metadata. The native app must still verify the
post-SRP P-256 transcript binding against its previously trusted key.

The runtime reaps crypto sessions after60 seconds of inactivity and closes
the matching service/transport route for every expiry or error. A successful
guest request also validates its current individual grant. Password rotation
or revocation closes guest crypto and BLE routes after the current owner
operation has produced its reply. A successful explicit close acknowledges
before terminating the transport.

## Validation boundary

`sh check.sh` runs the pure queue under ASan/UBSan, checking original deadlines,
boot mismatch, replay, one in-flight request, reserved owner capacity,
cross-transport rate limits and disconnect/close ordering. The separate pure
recovery suite checks backoff, blocked admission, one attempt per deadline,
successful reset and integer overflow without starting any task. Local-time
tests cover strict DNS/NTP parsing, hostile/truncated packets,2036 era rollover,
untrusted/backward time and per-peer/global response limits. A mocked IDF
adapter suite additionally checks AP-only binding, DHCP lease changes between
admission and send, the one-packet worker budget, socket-budget refusal,
conflicting DNS configuration, partial-open cleanup,30-second retry backoff,
quiesce closure and absence of synchronized replies without trusted UTC.
All host groups use ASan/UBSan. The isolated IDF
candidate must also compile the full runtime. These checks do not test actual
BLE radio timing, heap peaks, NVS power loss, physical bulbs or the display.
The combined hardware stress tests and60-minute soak remain mandatory before
production activation.

## AP-local bulb clock helper

The provisioned lighting worker owns two additional nonblocking UDP sockets,
bound to both the actual `WIFI_AP_DEF` interface and its IPv4 address: DNS53 and
SNTP123. No task, DHCP mutation, NAT, DNS forwarding or station-network listener
is added. Initialization changes only local memory. Both sockets close before
the runtime acknowledges quiesce/OTA, and on every partial-open or I/O failure.
Failed startup/rebinding retries at most once per30 seconds.

The pinned IDF6.1 DHCP server already advertises the AP address as DNS when no
explicit DNS override is set. The adapter reads both AP DHCP DNS entries and
refuses an unexpected external address; it never stops DHCP or clears leases.
It answers the exact, case-insensitive name `ntp.wiz.world` with the AP IPv4
address (A,30-second TTL). AAAA receives NOERROR with no answer, not invented
IPv6. Other names receive NXDOMAIN; nothing is forwarded or redirected to the
display site. Only one uncompressed IN question is accepted; EDNS, additional
records, compression pointers and packets over96 bytes are refused. An A reply
is47 bytes for a31-byte question; other DNS replies are the request's length.

Every response requires a current Wi-Fi association and DHCP lease matching
the source IPv4, checked again immediately before send. Per protocol, one MAC
gets at most one response per second, and all peers share an eight-response
sliding one-second limit. One packet total is consumed per worker iteration;
DNS and NTP alternate first chance. Malformed input is also charged to that
iteration. The SDK UDP receive mailbox is bounded at six packets per socket.
These bounds limit application work, not RF jamming, stack-level flooding or
spoofing by an already associated malicious peer; DHCP/MAC is not cryptographic
identity.

SNTP accepts only48-byte version3/4 client-mode requests and sends exactly48
bytes. It copies the client transmit timestamp into originate, uses independently
checked receive/transmit UTC samples, and sends the actual last received sync
anchor as the reference timestamp. Sample/reference/age must agree exactly,
and age cannot exceed24 hours. It advertises coarse one-second precision and a
root-dispersion allowance of one second plus1000ppm of anchor age, rounded
upward. This is an engineering allowance, **not** a measured bound on upstream
error or authenticated time. Stratum15 and reference ID0 conservatively avoid
claiming a known upstream hierarchy; this is not a full precision NTP server.
Strict NTP clients may reject an aged sample's dispersion, and actual WiZ
acceptance remains unqualified. The helper stays silent while the existing
trusted-time callback refuses UTC. No wall-clock fallback is used. NTP wire seconds wrap
modulo2^32 at2036; the single all-zero timestamp instant is refused as unknown.
Client-side era selection still depends on the client's implementation. Neither
DNS nor NTP here is authenticated: this restores a clock dependency for signed
WiZ requests, not a new security root. [RFC5905 packet and era semantics](https://www.rfc-editor.org/rfc/rfc5905.html),
[RFC4330 SNTP server guidance](https://www.rfc-editor.org/rfc/rfc4330.html)

The helper requires at least18 socket descriptors: HTTPS6 (three clients plus
three internal), arcade7 (four clients plus three internal), relay1, WiZ UDP1,
DNS/SNTP2 and one outbound release/check reserve. The isolated lighting candidate
uses20; the unchanged default build uses16 and is refused by this helper.
The gateway's DNS/SNTP clients use raw UDP PCBs, not socket descriptors. Budget
their PCBs separately: four HTTP control/message PCBs, up to four DNS-source
PCBs, DHCP client/server2, upstream SNTP1, WiZ1 and this helper2 total14 against
the candidate's16. These arithmetic bounds do not replace measured combined
heap and management-access checks. The helper adds504 bytes to the C3 runtime
context. With the final metadata ABI, compiler frames on the
`poll -> receive -> peer` path total896 bytes (80+464+352), before SDK/callback
frames; this is not a measured live stack watermark.

`lrt_status_t.local_time` reports `off`, `waiting_ap`, `socket_budget_refused`,
`dns_configuration_refused`, `bind_failed`, `waiting_trusted_clock` or `ready`,
plus saturating DNS/NTP reply, malformed, foreign-peer, rate-limit, clock-refusal
and I/O-error counters. Runtime `active` does not imply the helper is ready.
No bulb cold-start convergence has been proved by these host/C3 compile checks.
Qualify actual DNS requests, `getTime`, signed readback, simultaneous power loss,
clock expiry and OTA shutdown on the selected model before relying on recovery.

Pinned SDK implementation references: `components/lwip/apps/dhcpserver/dhcpserver.c`
(default DNS offer), `components/esp_netif/lwip/esp_netif_lwip.c` (AP DNS reads),
`components/esp_http_server/src/httpd_main.c` (three internal sockets), and
`components/lwip/lwip/src/{apps/sntp/sntp.c,core/dns.c}` (raw UDP clients).
