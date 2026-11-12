# Management API v1

## Display operations

Display operations use the same authenticated session, CSRF/Origin checks and
error model as firmware operations:

| Method and path | Request / result |
|---|---|
| `GET /api/v1/display` | Driver, saved content hash/size, playback counters, errors, panel settings |
| `PUT /api/v1/display` | JSON subset of `mode` (`test`, `play`, `pause`, `blank`, `replay`, `replay_pause`), `profile` (`red`, `green`, `black`), `rotation` (0–3), `inverted` (boolean), `replay_chapter` (`0` to restart the built-in loop) |
| `GET /api/v1/display/replay` | Authenticated binary ECL1 download for local preview; 409 during OTA, at most 12,288 bytes |
| `PUT /api/v1/display/clip` | `application/octet-stream`; complete validated ECL1 clip, maximum 12,288 bytes; replaces the saved clip after validation |
| `GET /api/v1/display/clip` | Binary ECL1 download; 404 if unavailable |
| `DELETE /api/v1/display/clip` | Explicit removal of the saved clip; does not erase the OTA journal or credentials |

Media/config mutations are rejected during OTA/trial boot. `GET /status` and
diagnostic exports include a `display` object. A successful clip upload does not
change the selected playback mode; the browser's **Save and play** performs hash
readback and then an explicit `mode: play` request. Identical clip uploads are
idempotent. After a lost upload response, read the saved SHA-256 before retrying.
See [ECL1 and the electrical/storage constraints](display.md).

The `display.replay` object reports `active`, `format`, `title`, source `width`
and `height`, asset `bytes`, `frame`, `frames`, `period_us`, completed `loops`,
`caption` and `caption_index`. The compatibility `chapters` field is `1`.
Restart with `mode: replay` and `replay_chapter: 0`; other chapter indices are
rejected. `caption` is the fixed room label `203`; `caption_index` remains `0`
for API compatibility. Pausing or restarting the loop does not alter the label.
The browser preview has its own local timeline; its controls are explicitly
separate from device playback. See [Neko packing and frame verification](neko.md).

## Firmware and management operations

`paused_for_trial` indicates the bounded station-association hold during an
OTA trial. The saved `enabled` setting remains unchanged.

Network status includes actual `station_mac` and `ap_mac` addresses read from
the Wi-Fi driver. Register `station_mac` with a campus captive portal. Both
`station_connected` and the legacy `online` field mean Wi-Fi association plus
DHCP, not proof of Internet access; `relay.connected` reports authenticated
connectivity to the arcade relay. The firmware uses factory-derived fixed MACs
and does not randomize or override them.

Relay status includes `resolved_addresses` (0–4) and a `resolving` state. DNS
success alone does not set `connected`; that requires authenticated WebSocket
admission. An address failure advances to another current DNS answer on the
next bounded retry, retaining TLS hostname verification.

Base URL: `https://192.168.4.1/api/v1`. There is no plaintext management endpoint. Responses are JSON except bundled assets and binary clip downloads. Errors have `error` and `message` strings. Authentication errors are 401, origin/ticket failures 403, throttling 429, unknown operations 404 and rejected state/image/settings operations 409. Rejected requests close their connection so unread bodies cannot be mistaken for a subsequent request.

JSON requests use exactly `Content-Type: application/json`, at most 2048 bytes, unique keys and a flat object. Chunk bodies use `application/octet-stream`, at most 8192 bytes. All operations are local to the device; there is no Internet-facing listener configuration.

| Method | Path | Request / result |
|---|---|---|
| POST | `/session` | `{ "password": "…" }` → session `token`, `csrf`, secure cookie |
| DELETE | `/session` | Revoke the current session |
| GET | `/status` | Version, slot, heap, reset reason, settings, network and OTA state |
| GET | `/network` | Hotspot and station status; never passwords |
| PUT | `/network` | `{ "ssid": "…", "password": "…", "enabled": true }` |
| POST | `/ota/start` | `{ "size": 1118208, "sha256": "64 hex digits" }` → `ticket`, `chunk_size` |
| PUT | `/ota/chunk` | Binary body, `X-Upload-Offset` and `X-Update-Ticket` headers |
| POST | `/ota/finish` | `X-Update-Ticket`; verify, activate and reboot |
| POST | `/ota/abort` | Cancel an incomplete transfer |
| GET | `/ota/status` | Progress, target/running versions, trial and health status |
| POST | `/ota/confirm` | `X-Update-Ticket`; record updater reconnection |
| POST | `/ota/rollback` | Restore a valid previous image and reboot |
| GET | `/releases` | Check/install state and verified release metadata |
| POST | `/releases/check` | Queue a release check; requires an online uplink |
| POST | `/releases/install` | Install the verified release → `ticket`; never automatic |
| GET | `/logs` | Up to 64 bounded diagnostic events |
| GET | `/diagnostics` | Download a JSON snapshot with events and release state |

Browser mutations require both the session cookie and `X-CSRF-Token`. Supplied Origin headers must match the HTTPS Host. CLI requests use `Authorization: Bearer <token>`. Two simultaneous sessions are supported, with a one-hour expiry; a new session replaces the oldest. Reboot clears every session. Five failed login attempts within a minute cause throttling.

`/ota/status` and `/ota/confirm` also accept the scoped update ticket without a session, allowing automatic reconnection after reboot. Other operations require administrator authentication. A ticket cannot start a transfer, access credentials, or modify networking. Upload chunks also require a valid session.

An OTA installation is successful only when `/ota/status` reports `phase: "confirmed"` and the expected `running_version`. Upload completion, HTTP 200, reboot, and the presence of the hotspot are not confirmation. Phases include idle, receiving, verifying, rebooting, trial, confirmed, failed, rolling_back and rolled_back. A host-verified physical installation can report `usb_installed`; that is not OTA confirmation. Always surface the device's `detail` text when an operation fails.

After a lost chunk reply, query the ticket's `/ota/status`. While `phase` is `receiving`, `received` equal to the chunk end acknowledges that write; `received` equal to its starting offset permits retry. Reject any other offset or phase. The supplied clients bound write attempts and status recovery to three attempts each. Restart from `/ota/start` after an aborted or timed-out transaction. Never blindly replay an uncertain mutation.

During trial boot, send `/ota/confirm` every two seconds until the expected image reports `confirmed`. The most recent request must be within ten seconds of completion of the continuous 30-second health window; a single early request is insufficient. The 120-second deadline still applies.

An explicit release check may supply `{ "version": "0.1.0-rc.74" }` to verify that version's public assets, including a GitHub qualification release. The authenticated request is still constrained to the configured repository and signed manifest; arbitrary URLs are never accepted. Automatic checks always return to the latest stable release. Images retain the strictly-newer version policy.
