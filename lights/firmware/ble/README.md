# Lighting Bluetooth transport candidate

This component and the native macOS byte channel are **not enabled in the
production display firmware**. Source/host tests and a successful build do not
prove C3 radio coexistence, available heap, end-to-end authentication or bulb
control. The installed firmware remains the acceptance baseline.

The adapter owns the otherwise unused NimBLE peripheral host. It starts only
when explicitly requested after credentials/application service initialization.
No camera, display prompt, OS pairing PIN, Wi-Fi change, or plaintext password
is part of this transport. Application authentication uses the separate
Security2/session adapter and pinned gateway identity.

## GATT contract

| Object | UUID | Properties |
|---|---|---|
| Service | `89f5e330-0ba9-4d7d-b7e4-320303000001` | Primary |
| RX | `89f5e330-0ba9-4d7d-b7e4-320303000002` | Write **with response** |
| TX | `89f5e330-0ba9-4d7d-b7e4-320303000003` | Indicate |

Subscribe to TX before writing RX. Service discovery/advertisements are only
untrusted connection hints. The server advertises `203 Lights` at 150–200 ms
intervals and requests ATT MTU 128. Client writes use
`min(maximumWriteValueLength(.withResponse),512)` bytes. The default ATT MTU 23
also fits the 12-byte header, albeit with much slower fragmentation.

Every characteristic fragment starts with this network-byte-order header:

| Byte | Field |
|---|---|
|0|Version 1|
|1|Operation: 0 identity, 1 session, 2 data, 3 close; response sets bit 7|
|2|Flags: bit 0 START, bit 1 END; no other bits|
|3|Reserved 0|
|4–7|Nonzero uint32 request ID, strictly increasing per connection|
|8–9|Complete message size, uint16|
|10–11|Byte offset into complete message, uint16|
|12…|Fragment bytes|

Requests contain at most 1024 payload bytes. Only `close` permits an empty
payload. A response message starts with its uint16 status followed by at
most 1024 payload bytes, so total response capacity is 1026. Success is status 200;
other 200–599 values fail the exchange and require a fresh secure connection.
The transport does not make a status 200 into a confirmed bulb change.

Offsets must be contiguous. START occurs exactly once at offset 0; END occurs
exactly when the accumulated size reaches the advertised total. A repeated ID,
overlap, missing/extra START/END, changed operation/size, malformed field or
oversized fragment disconnects the link. Each link permits one request at a
time. A late response never completes a newer request.

## Bounded work and lifecycle

- Two BLE links, each with one 1056-byte receiver; one shared 1026-byte response
  buffer. A worker-owned `lb_event_t` is separate and reused across calls.
- GATT callbacks only copy, validate fragments, update small counters and
  schedule disconnection. They do not execute SRP, parse JSON, persist NVS or
  contact bulbs. The existing application worker polls at least every 50 ms.
  Register it with `lb_transport_set_worker` to receive immediate task
  notifications on completed requests, disconnects and indication ACKs; those
  notifications prevent adding a full polling interval to every fragment.
- The worker handles `lb_transport_take`, dispatches the original route and
  deadline, then calls `lb_transport_reply`. Disconnected-route events precede
  new requests and must close the matching Security2 context. Ready requests
  alternate between links, so one busy link cannot continually jump the queue.
- Ten seconds cover assembly, dispatch and complete response transmission.
  Only one indication is outstanding, with a three-second acknowledgement
  deadline. No unbounded notification queue or ciphertext retransmission.
- Pre-authentication limit: 6 messages and 12 KiB per 10 seconds per link. After the
  application validates its per-install grant, limit 40 messages / 24 KiB. The
  common authority layer must still apply its cross-transport rate policy.
- Peers that have not obtained a valid application grant expire 10 seconds after connecting,
  regardless of current RX/response phase; identity polling never extends this
  deadline. Authorized idle links expire after five minutes (the secure runtime
  applies its shorter 60-second session expiry). BLE slot saturation/RF jamming
  cannot be prevented by these
  application limits; do not claim guaranteed owner radio availability.
- `lb_transport_stop` disconnects and stops/deinitializes NimBLE without the
  irreversible controller-memory-release API. Consume pending disconnected
  events before restarting. Repeated stop/start needs actual hardware tests.

`BluetoothByteChannel` in `lights/macos` implements the matching native
CoreBluetooth protocol. Construction is inert; `open(peripheralID:)` requests
Bluetooth access in context, uses targeted service discovery, checks RX/TX
properties, and subscribes before accepting an exchange. Every write waits for
its ATT response; every completed reply waits for both final write completion
and the entire matching indication sequence. Cancellation/timeouts close the
channel; the application must perform fresh SRP on failover. No Wi-Fi changes,
credential handling or automatic policy authority occur in this byte layer.

## Validation

Run `sh check.sh` for the ASan/UBSan C suite, including replay/order/truncation,
deadline/size bounds, response fragmentation for 20…512-byte characteristics,
and 100,000 deterministic malformed packets. Run `sh check-swift.sh` for native
encoding/assembly checks without using a radio. The C and Swift golden request
is `01 01 03 00 00 00 00 01 00 03 00 00 01 02 03`.

Before enabling this component, the combined candidate must pass its signed
image/heap gates, Security2+identity binding interoperability, five-bulb plus
display/game traffic, two-client BLE admission/flood tests, and the 60-minute
soak. ESP-IDF 6.1 labels C3 connected SoftAP plus BLE as supported with unstable
performance; compile success is not evidence that this combination is safe for
the existing display. [Espressif coexistence table](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-guides/coexist.html)
