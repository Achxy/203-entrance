# Browser gateway security

This ESM module connects ordinary HTTPS browsers through the existing public
WebSocket relay. Bluetooth is not required. It never changes phone/laptop Wi-Fi
or reaches bulb UDP endpoints directly. Live control still requires the gateway
and its campus uplink to be online.

## Public API

```js
import {
  GatewayWebSession, parseTrustedDescriptor, deriveGuestVerifier, randomHex
} from './security/gateway-web-session.mjs';

// Read from an owner-verified descriptor file, not public relay discovery.
const descriptor = parseTrustedDescriptor(fileText);
const session = await GatewayWebSession.connect({
  descriptor, role: 'guest', credential: guestPassword,
  onState: state => showConnectionState(state)
});
// Clear the UI's password input and its temporary credential reference now.
// No light command or grant enrollment occurs merely by connecting.
const result = await session.request({v: 1, op: 'room'});
session.close();
```

`connect` also accepts an optional AbortSignal for the connection attempt.
Its states are `connecting`, `authenticating`, `binding`, `ready`, `closed`.
`session.role` is absent until the credential role is signed by the trusted
gateway and becomes absent again on close. Relay role fields never grant access.
`session.boot` is authenticated by that same signature. `session.isOpen` reflects
the channel and bound role. Leaving the page (`pagehide`) closes the session;
restoring the page requires a fresh explicit connection.

Descriptor JSON has exactly these six fields:

```json
{
  "v": 1,
  "gateway_id": "203-c7f088",
  "public_key_spki": "<182 lowercase hex characters: P-256 SPKI DER>",
  "relay_url": "wss://your-relay.example/v1/lights",
  "security": 2,
  "patch": 1
}
```

The schema validates a standard uncompressed P-256 SPKI, secure relay URL, and
the supported protocol. Validation cannot establish where the file came from.
Never automatically trust a key learned from `/info`, a WebSocket reply, a QR
advertisement, or an unverified upload. A boot ID is transient and deliberately
absent from the permanent descriptor. Key replacement needs explicit owner trust.

Every application request must include `v:1` and one supported `op` from
`lights/firmware/gateway/PROTOCOL.md`. `request` returns the decrypted strict JSON
reply; `ok:false` is an authenticated application rejection, not a reason to retry.
The server remains the role, permission, rate, revision, and deadline authority.
Concurrent requests fail closed instead of forming a queue.

The UI controls explicit guest enrollment/resume. `randomHex(32)` makes a grant
token and `randomHex(16)` makes a command UUID. The browser module stores neither
passwords nor tokens in localStorage, cookies, IndexedDB or a service worker.
Keep them in memory only, clear on logout, and never silently reenroll a revoked
guest. A previously accepted command keeps its original ID during reconciliation;
the module does not mint a new command, automatically retry a write, or resume old
ciphertext after failure.

Owner password rotation uses `await deriveGuestVerifier(newGuestPassword)` and
sends `{v:1,op:'rotate_guest',...result}` only over the already owner-bound channel.
The helper generates a fresh 16-byte salt and 384-byte padded SRP verifier for
username `203-guest`; it does not send or retain the new password. No verifier
generator call performs a network request.

## Protocol and transport invariants

- Only Security2 patch 1 is accepted. There is no Security0, Security1, plaintext,
  or unsigned identity fallback. The two SRP proofs must match before binding.
- Four exact protobuf records are length-prefixed and SHA-256 hashed. The gateway
  signs the gateway ID, boot, this transcript, a fresh challenge and the proven
  credential role. No application record can be encrypted until that trusted
  P-256 signature verifies.
- One shared AES-GCM nonce counter is used in both directions: binding request 1,
  binding response 2, first app request 3, first app response 4. A timeout, close,
  replay, failure, concurrent request or path change destroys the session; it is
  never recovered by guessing which nonce the other side consumed.
- Canonical protobuf rejects duplicates, unknown fields, wrong oneofs, overlong
  varints, default-field aliases, truncation and oversized records. The maximum
  handshake/ciphertext is 1024 bytes, plaintext 1008 bytes, relay envelope 2048
  bytes. JSON rejects duplicate decoded keys, unsafe numbers, invalid UTF-8,
  ambiguous numeric encodings, excessive nesting and trailing data. Binding and
  relay objects are flat; binding fields are unescaped ASCII and exact.
- The relay's connection UUID, boot, monotonically increasing request ID and
  expected envelope type are checked on every response. The 3-second local data
  response timeout does not extend the relay/device's original 2-second command
  lifetime. Handshake response timeouts are bounded separately. No ciphertext is
  retried or switched to a new socket.
- All relay sends, including SRP and binding, are at least 450 ms apart. The client
  reserves one exchange during pacing, waits before encrypting an app record,
  and cancels the wait on close. This is flow control for multiple guests, not
  a replacement for gateway/server rate limits or owner precedence.
- The caller must read fresh gateway monotonic time and revisions before forming
  a `set`. The supplied boot must match the bound connection. A transport ACK
  does not prove a light state: poll the original `command` receipt, then fetch
  the current light's actual observation.

Modern browsers need WebCrypto SHA-512/SHA-256, AES-GCM-256, ECDSA P-256 SPKI import,
native BigInt, secure random bytes, fatal UTF-8 decoding, ESM and WebSocket. Missing
features fail closed. This uses the same Internet path on Android and iPhone; it
does not claim that Safari supports Web Bluetooth. WebCrypto is exercised by
the included browser harness, not replaced with a browser-name assumption.

## Checks and deployment boundary

From the repository root:

```sh
node --test sites/lights/src/security/tests/security.test.mjs
python3 sites/lights/src/security/tests/check_upstream.py
```

Serve the site over local loopback HTTP or HTTPS and open
`/src/security/tests/browser-check.html` in each target browser. It runs only
public test fixtures: no gateway requests, credentials, pairing, or bulb writes.
Record the resulting assertions and user agent separately from real hardware
qualification. The standalone `connectForTesting` hook injects a fake transport
and deterministic randomness for the Node harness; product UI must use `connect`.

Production deployment must exclude this directory's `tests/`, fixtures, source
pin check scripts and documentation from runtime assets. Keep the license notices
in the distributed source/license bundle. The root site owns its CSP, relay Origin
allowlist, hosting and DNS; this module changes none of those settings. No browser
or hardware result is inferred from passing the Node harness.

See [NOTICE.md](NOTICE.md) for exact references, attribution, variable-time SRP
limitations, memory-erasure limits and the independent-review boundary.
