# Lighting relay protocol v1

Status: implemented and host-tested; disabled by default; not deployed or
hardware-qualified. The relay carries opaque application frames, not WiZ UDP,
arbitrary HTTP, OTA, Wi-Fi administration, credentials, or URLs. Lighting shares
the existing outbound authenticated device WebSocket. It creates no additional
ESP TLS connection and does not alter the arcade messages.

## Enabling and discovery

The control WebSocket returns 404 unless `LIGHTS_ENABLED === "true"`. This exact
string is required. The committed Wrangler configuration is `"false"`.

- `GET /v1/lights/info` returns the public descriptor below, including during a
  disabled staged rollout. While disabled, `online` is always `false`, even if
  the display is connected or advertises a lighting capability. The Worker
  returns this static public identity without accessing a Durable Object,
  opening sockets, creating lighting tables or consuming admission budgets.
  No WebSocket upgrade is required. `Cache-Control: no-store` prevents stale
  online state.
- `GET /v1/lights` requires `Upgrade: websocket`. No query parameters or other
  HTTP methods are accepted.

Native clients may omit `Origin`. If supplied, it must exactly match an entry
in `ALLOWED_ORIGINS`; an empty or `null` origin is not a native-client bypass.
This exception does not apply to existing browser game endpoints. The public
descriptor includes a CORS allow-origin header only when an origin was supplied.

```json
{
  "protocol": 1,
  "online": false,
  "gateway_id": "test-gateway",
  "public_key": "-----BEGIN PUBLIC KEY-----\n...\n-----END PUBLIC KEY-----\n"
}
```

`public_key` is the existing device P-256 SPKI PEM verification key. It is public
identity material, not a credential. The endpoint returns no configuration,
bulb inventory, role, WiZ signing key, token, or stored light state. Fetching this
key does not by itself authenticate a Security2 channel: clients must verify a
trusted pin and a gateway proof bound to the SRP transcript, fresh client nonce,
transport and connection. Shared guest-password knowledge alone must not let a
guest impersonate the gateway. The client/security adapter owns that proof.

A device advertises readiness in its existing authenticated pulse:

```json
{"v":1,"type":"pulse","lease":123456,"light_protocol":1}
```

The device must implement the complete bounded gateway/security integration
before advertising `1`. No capability, `0`, another value, a heartbeat older
than 1.5 seconds, or no authenticated device makes lighting unavailable. Setting
the server flag cannot make an old device lighting-capable. Existing pulse
fields and arcade timing retain their meanings. Lighting replies do not refresh
the device heartbeat or consume the existing game/device input-rate budget.

## Native WebSocket

Admission requires an available gateway. The first server message is:

```json
{"v":1,"type":"light_online","online":true,"connection":"00000000000000000000000000000001","boot":"aaaaaaaaaaaaaaaa","protocol":1}
```

This means only that transport is available. It does not authenticate a user.
`connection` is a fresh 128-bit lowercase hexadecimal relay identifier. `boot`
is the current device's 64-bit lowercase hexadecimal boot identifier.

Clients send text JSON frames only. Flat scalar fields are required, duplicate
keys and unknown fields are rejected, and the entire UTF-8 frame is at most
2048 bytes:

```json
{"v":1,"id":1,"op":"session","payload":"AQID","role_hint":"guest"}
```

| Field | Requirement |
| --- | --- |
| `v` | Integer `1` |
| `id` | Integer `1..4294967295`, strictly increasing within this connection |
| `op` | `identity`, `session`, `data`, or `close` |
| `payload` | Canonical padded RFC 4648 Base64, at most 1024 decoded bytes |
| `role_hint` | Optional `unknown`, `owner`, or `guest`; absent means `unknown` |

Only `close` has an empty payload, and its payload must be empty. The other
operations require nonempty opaque bytes. Base64 whitespace, URL-safe spelling,
omitted padding, nonzero padding bits, nested objects and binary WebSocket
frames are invalid. There is no arbitrary method, path, UDP payload, host or
URL field. `role_hint` is an untrusted verifier selector; it never grants a
  role, priority, admission or an administrative operation.

- `identity`: gateway identity proof exchange defined by the security adapter.
- `session`: exactly one Security2 handshake frame.
- `data`: exactly one Security2 encrypted application record. The gateway must
  already have authenticated this connection before the relay forwards data.
- `close`: release the gateway session and acknowledge it, then close transport.

There is exactly **one outstanding request per client**. Do not send the next
frame until the matching response is verified. Coalesce UI slider changes
before encryption into the latest desired state; do not enqueue encrypted
slider events. The relay does not interpret or rewrite application records.

## Device multiplexing

The relay forwards a lighting request on the existing device socket:

```json
{"v":1,"type":"light_request","connection":"00000000000000000000000000000001","id":1,"op":"session","payload":"AQID","role_hint":"guest","boot":"aaaaaaaaaaaaaaaa","lease":123456,"ttl_ms":10000}
```

The device must accept only this exact bounded lighting grammar and enqueue work
in its separate lower-priority lighting lane. It must not execute lighting
parsing, SRP, socket waits or bulb discovery in the display/game task. The relay
never consumes arcade request credits for lighting, but physical CPU/radio/heap
coexistence still requires firmware scheduling and hardware qualification.

`lease` is the last observed device monotonic pulse value. The device must reject
the request if its boot differs or its current monotonic clock is at or beyond
`lease + ttl_ms`. **Do not start a new TTL on receipt:** a TLS frame may have
arrived after the relay timed out. Data/close TTL is at most 2000 milliseconds.
Identity/session TTL is the remainder of the connection's total 10-second
authentication budget, never more than 10000 milliseconds. The gateway must
also enforce authenticated application TTL, sequence, policy epoch and UUID
deduplication; these outer relay fields are not cryptographic authorization.

The complete forwarded frame, including a maximum-size Base64 payload and
routing metadata, is checked to fit the firmware's 2048-byte input budget.

The device responds only to a request:

```json
{"v":1,"type":"light_reply","connection":"00000000000000000000000000000001","id":1,"boot":"aaaaaaaaaaaaaaaa","status":200,"payload":"BAUG","role":"guest"}
```

The relay accepts only exact fields `v`, `type`, `connection`, `id`, `boot`,
`status`, `payload`, and optional `role`. Status is an integer `200..599`, but
only `200` leaves the session open. Payload uses the same Base64/1024-byte bound;
successful non-close replies must be nonempty. Device replies are matched to a
current unexpired pending request, connection and current device boot. Wrong,
late, duplicated and orphan replies are never delivered to another client.

The optional `role` is `owner`, `guest` or `none`:

- Omit it during intermediate handshake steps.
- Assert `owner` or `guest` only after the gateway verifies the user and its
  policies. The final SRP client-proof handshake reply can assert the proven
  credential role, allowing the following encrypted channel-binding `data`
  request. Only a successful `session` or `data` reply can carry those roles.
- `none` explicitly revokes/ends the session; it is not an intermediate
  unauthenticated-handshake marker.

The relay trusts this role for transport priority only because the assertion
came from the authenticated device and matched a pending request. The native
client must still complete its own Security2 and gateway-identity checks. Role
labels on an unencrypted relay envelope are not end-to-end authentication.
There are at most two authenticated guest sockets, leaving capacity for owner
authentication. This aligns with the planned gateway pool of two guest sessions
and one reserved owner session. The relay may admit an additional unauthenticated
transport, but the gateway must enforce its own stricter crypto pool capacity.

### Transport cleanup

When a lighting client disconnects, errors or expires, the relay best-effort
sends this device-only lifecycle message if the same authenticated device boot
is still connected:

```json
{"v":1,"type":"light_disconnect","connection":"00000000000000000000000000000001","boot":"aaaaaaaaaaaaaaaa"}
```

There is no payload, request ID, role, acknowledgement or authority to alter a
light. The gateway validates the exact fields, current boot and known connection,
cancels queued work for that connection, invalidates its session generation and
wipes the matching crypto session. A cancellation cannot undo a bulb command
that already executed. Ignore unknown connections. This lifecycle message is
not a Security2 record and does not advance a cipher nonce; it may cancel a
request that was in flight when the transport died. Native clients cannot
choose another connection or send this device message themselves.

Cleanup is emitted at most once per admitted client; a persisted attachment
flag prevents duplicates from successive close/error callbacks. Global
admission is capped at 24 new connections per 60-second admission window, with
at most four existing connections carried into a window, bounding cleanup work
independently of the eight opaque requests/second. A successful explicit `close`
acknowledgement does not send redundant cleanup. No lifecycle frame is stored
for future delivery, and a later device boot never receives old cleanup.

## Errors, expiration and recovery

For a pending or rejected request, relay errors use this envelope followed by
WebSocket close. Empty payloads are transport errors, never fabricated encrypted
Security2 responses:

```json
{"v":1,"type":"light_reply","connection":"00000000000000000000000000000001","id":1,"boot":"aaaaaaaaaaaaaaaa","status":504,"payload":"","error":"timeout"}
```

If no request is pending, there is no invented ID or duplicate reply:

```json
{"v":1,"type":"light_offline","online":false,"connection":"00000000000000000000000000000001","boot":"aaaaaaaaaaaaaaaa","error":"offline"}
```

Errors include `offline`, `unavailable`, `gateway_replaced`, `timeout`, `expired`,
`replay`, `in_flight`, `busy`, `rate`, `authentication_required`,
`new_connection_required`, `invalid_gateway_reply`, `invalid_gateway_payload`
and `invalid_gateway_role`. Invalid client grammar may close directly without a
reply. Unknown future error codes must fail closed. Do not infer that a timed
out command did or did not affect a bulb; reauthenticate and read current state.

Security2 patch 1 has one shared request/response nonce sequence. Any timeout,
transport rejection, disconnect, malformed record, replay, authentication/tag
failure or route change must destroy that crypto session. Never retransmit old
ciphertext, move it to BLE, or continue the old nonce state after reconnecting.
Open a new connection and complete fresh SRP and channel identity verification.
The relay sends no unsolicited encrypted device pushes. Gateway application
responses should keep business errors inside valid encrypted replies when the
cryptographic session remains usable.

The relay stores no commands for offline delivery. Only minimal pending routing
metadata lives in WebSocket attachments; payloads are not persisted to SQLite.
Device replacement, disconnect, boot mismatch or capability withdrawal closes
all lighting sockets. Malformed lighting replies terminate the lighting lane
without terminating the authenticated device/game socket. Gateway session pools
must independently reap orphan sessions: a dead transport cannot reliably send
the explicit `close` operation or the best-effort lifecycle message.

## Independent budgets

| Budget | Limit |
| --- | --- |
| Lighting sockets | 4 total, at most 2 not yet authenticated, at most 2 authenticated guests |
| Pending frames | 3 total, at most 2 non-owner; at most 1/client |
| Forwarded frame rate | 8 in any rolling second; at most 6 non-owner |
| Handshake budget | Total 10 seconds from connection creation |
| Preauth operations | 6 per peer / 10 seconds; 12 aggregate / 10 seconds |
| Connection attempts | 8 per peer / minute; 24 aggregate / minute |
| Data/close deadline | At most 2 seconds |
| Authenticated inactivity | 60 seconds |
| Absolute connection lifetime | 30 minutes, then fresh session |
| Per-peer persisted rate rows | 128, in separate `light_limits` table |

At least one pending credit and two operations/second remain reserved for a
**proven** owner when guests are busy. Unauthenticated clients cannot claim these
credits by selecting the owner verifier. Shared-NAT admission limits necessarily
apply to users behind the same public IP. Transport budgets bound ordinary
abuse; they do not promise immunity to radio jamming, campus outages or
unlimited volumetric traffic. Client sliders must coalesce before issuing new
requests and handle rejected transport by recreating the session.

Lighting admission rows, rate counters, pending credits and device replies are
separate from existing arcade and device recovery budgets. Attachments and
SQLite admission counters survive Durable Object hibernation. Alarms schedule
the nearest pending deadline and expired frames are rejected again when a reply
arrives; correctness does not rely solely on alarm punctuality.

Two non-owner pending credits let both authorized guests control lights without
forcing each other to redo SRP on ordinary overlapping requests. The third
credit remains owner-reserved. Native slider output should be coalesced to at
most two writes/second per user, with polls no more often than once/five seconds
and paused during writes. The gateway must still bound memory to its fixed
three-session pool and one pending record/session rather than building a
second queue behind the relay credits.

## Validation boundary

On 2026-11-16, after the disabled-rollout readiness change, `npm test` passed
45 tests (30 lighting and all 15 existing
arcade tests), `npm run check` and `git diff --check -- relay` passed, and
Wrangler 4.160.1 completed a local `deploy --dry-run` bundle of 30.85 KiB
(8.17 KiB gzip). The dry run reported `LIGHTS_ENABLED: "false"`. No Worker
deployment, remote variable change, device flash or light operation was made.

`npm test` covers the original 15 arcade tests plus lighting route gating,
origin policy, descriptor visibility, capability advertisement, socket limits,
isolated rate storage, strict parsing, Base64 bounds, maximum wire size,
untrusted role hints, owner capacity, independent arcade credits, one in-flight
request, request replay, reply routing/boot, role assertions, offline rejection,
handshake/data deadlines, hibernation, rate limits, malformed gateway replies,
device replacement/disconnect, capability withdrawal, close, session expiry,
guest capacity and exactly-once, boot-bound lifecycle cleanup.

These host tests use simulated sockets and SQLite. They do not establish actual
Cloudflare socket ordering, Security2 cryptographic correctness, WiZ behavior,
bulb pairing, hardware memory/radio stability, native UI behavior, or production
availability. Enabling and deploying this lane remains contingent on the
separate gateway/client integration and hardware acceptance evidence.
