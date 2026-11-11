# Arcade API v1

All JSON replies use `Cache-Control: no-store`. Errors contain a human-readable
`message`; device errors additionally carry a stable `error`. Controller tokens
are never administrator tokens. Input is relative motion, not a browser-owned
position or score. The physical display is authoritative.

## Local controller (`http://192.168.4.1`)

Only requests arriving on the AP interface with the exact local Host are
accepted. POST requires Origin `http://192.168.4.1` and JSON. The maximum body is
1,024 bytes. Authenticated controller requests use `X-Arcade-Token`.

| Method/path | Body or result |
|---|---|
| GET `/play`, `/play.js`, `/play.css`, `/relay.js` | Bundled offline controller |
| GET `/arcade/status` | Protocol, boot ID, uptime, stage, game, player slots, readiness, deadlines; with token, own slot/sequence |
| POST `/arcade/knock` | `{}`; shows physical pairing code, returns expiry only |
| POST `/arcade/join` | `{"code":123456,"profile":"G1.… or E1.…"}`; returns token and signed receipt |
| POST `/arcade/command` | `{"sequence":1,"action":0,"value":0,"axis":0}` |
| GET `/arcade/receipt` | Own receipt while grant is retained |

Actions: 0 input, 1 select game, 2 ready, 3 menu, 4 leave, 5 heartbeat.
Games: 0 Tetris, 1 Pong, 2 Pac-Man. Ready value is 0/1. Input bits: left1,
right2, up4, down8, rotate16, hard drop32, hold64. Pong axis ranges −1000…1000.
Only a new sequence is accepted. A 409 means an action is unavailable/stale; 429
means rate-limited. Resynchronize with status rather than replaying stale input.
Compare boot ID and uptime when accepting asynchronous status responses.

## Owner API (existing pinned HTTPS session)

| Method/path | Purpose |
|---|---|
| GET `/api/v1/arcade` | Runtime health, session state, guest network flag, log/overwrite counts |
| PUT `/api/v1/arcade/network` | `{"enabled":true}` enables public guest SSID; STA pauses only when relay is disabled |
| GET/PUT `/api/v1/arcade/relay` | Status / `{"host":"relay.example","enabled":true}`; owner-only, rejects OTA conflicts |
| GET `/api/v1/arcade/pairing` | Current code, for owner diagnosis |
| POST `/api/v1/arcade/release` | End the current turn and clear pairing banner |
| GET `/api/v1/arcade/visits` | Last eight signed receipts, newest first |
| GET `/api/v1/soak` | Bounded RAM history of minute health/heap/config samples; resets on reboot |

OTA and network changes remain serialized. The guest network setter rejects
conflicts with an update and applies its change after replying. Credentials
for optional station mode are retained; provisioning/partition replacement are
outside this interface.

The CLI uses the same certificate-pinned administrator session:

```sh
./entrance arcade status
./entrance arcade release
./entrance relay
./entrance relay --enable --relay-host 203-arcade-relay.achyuthanugraha.workers.dev
./entrance relay --disable
```

`--host` selects the reachable device address, not the public relay. Disabling
the relay retains its hostname. `arcade pairing` and `arcade visits` print
private admission data; keep their output out of public logs. These commands
do not join or change the computer's Wi-Fi network.

## Public HTTPS server

| Method/path | Purpose |
|---|---|
| GET `/api/config` | Public Clerk configuration, relay endpoint and device readiness |
| POST `/api/check-in` | Consent/name/room plus optional Clerk bearer; signs bounded identity pass |
| POST `/api/visits` | `{"receipt":"R1.…"}`; validates device admission, idempotently stores and syncs to Clerk |
| GET `/api/visits` | Owner-only recent private log and bounded retention/sync maintenance |

Browser writes require a same-site Origin and JSON, with a 4,096-byte limit.
No names, rooms, pass strings, receipt strings, admin passwords or private keys
belong in access logs or analytics. `/return` consumes a fragment receipt, clears
the URL, and retains a local retry copy until the server accepts it.

## Internet controller

`wss://203-arcade-relay.achyuthanugraha.workers.dev/v1/player` accepts only the
configured HTTPS site Origins. The browser receives `online` clock leases and
sends an exact flat envelope (no extra or duplicate fields):

```json
{"v":1,"type":"request","id":1,"op":"status","token":"","body":"{}","lease":0}
```

Operations have the same bodies and semantics as local routes. `body` is a JSON
string containing a strict flat object. IDs increase per socket. Replies carry
`v`, `type:"reply"`, matching `id`, HTTP-style `status`, and JSON-string `body`.
Do not log bearer tokens, profiles or signed receipts. A live token can resume
through a status request, but the original board deadline is never extended.

`GET /v1/status` returns public online/protocol state with an allowed Origin.
`/v1/device` is reserved for signed device authentication. It is not an owner
management tunnel. The relay never grants access to OTA or network settings.
