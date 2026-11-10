# Management API v1

Base URL: `https://192.168.4.1/api/v1`. There is no plaintext management endpoint. Responses are JSON except bundled assets. Errors have `error` and `message` strings. Authentication errors are 401, origin/ticket failures 403, throttling 429, unknown operations 404 and rejected state/image/settings operations 409. Rejected requests close their connection so unread bodies cannot be mistaken for a subsequent request.

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

An installation is successful only when `/ota/status` reports `phase: "confirmed"` and the expected `running_version`. Upload completion, HTTP 200, reboot, and the presence of the hotspot are not confirmation. Phases include idle, receiving, verifying, rebooting, trial, confirmed, failed, rolling_back and rolled_back. Always surface the device's `detail` text when an operation fails.

The inactive slot is not resumable: retry from `/ota/start` after an aborted or timed-out transfer. Do not blindly retry a mutation after a lost response; query status to determine whether it took effect. The supplied CLI implements the confirmation loop.

An explicit release check may supply `{ "version": "0.1.0-rc.74" }` to verify that version's public assets, including a GitHub qualification release. The authenticated request is still constrained to the configured repository and signed manifest; arbitrary URLs are never accepted. Automatic checks always return to the latest stable release. Images retain the strictly-newer version policy.
