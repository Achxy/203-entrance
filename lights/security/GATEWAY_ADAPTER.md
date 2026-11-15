# Native gateway application adapter

`lights/macos/Sources/LightsMenulet/GatewayAdapter.swift` implements the native
`MenuletTransport` over `SecureGatewayPipe`. The pipe performs fresh Security2
authentication and the pinned gateway's signed transcript binding before the
adapter sends any application request. Application JSON is validated against the
bounded gateway contract in `lights/firmware/gateway/PROTOCOL.md`; duplicate keys,
unknown fields, malformed values, and unexpected boot identifiers fail closed.

The adapter's constructor defaults to disabled. The native app's reviewed wiring
can enable it, but remains disconnected until the user selects Connect. A route
factory supplies actual readiness and an explicitly selected BLE peripheral;
constructing the adapter itself opens no socket, scans no radio, and reads no
Keychain record. A public device-key pin is trust input, not a secret or evidence
that the gateway is currently online.

## Authority and storage

Owner and guest usernames are exactly `203-owner` and `203-guest`. Guest access
enrolls a random per-install token or resumes its existing token immediately
after binding, before paginating room state. A pending enrollment may reuse the
same token, but an active, revoked, expired, or rejected token is never silently
replaced. Explicit Rejoin is a separate user action requiring the current shared
password. Revocation cannot prevent someone who still knows that password from
explicitly enrolling again; rotating it revokes that knowledge.

Only explicit Remember Access saves the owner key or guest password and grant to
non-synchronizing Keychain. Volatile password bytes permit a fresh session during
authorized path changes and are released on disconnect. Sign Out attempts to
delete every remembered secret and surfaces any Keychain deletion failure for
retry. The WiZ home key is submitted only inside an authenticated owner request;
this adapter does not retain it in Keychain. Swift copies cannot be guaranteed
fully zeroized; see the package's separate memory-handling limitations.

Owner password rotation generates a native-compatible 16-byte salt and padded
384-byte verifier off the main actor. Plaintext guest passwords are not sent to
the gateway. Freeze, revoke, remove, home-key import, bounded discovery, and
explicit candidate adoption use the gateway's implemented operations. A standalone
owner hold is explicitly unsupported; a hold can accompany a subsequent new
lighting change without rewriting old cached values.

## Commands, reconnects and truthful status

Only one encrypted channel is active at a time. Every RPC is paced by the pipe
at a minimum of 500 ms. Before the 60-second server idle deadline, the next user
operation renews a session idle for 55 seconds using fresh SRP on the available
path and resumes the same grant. A real transport failure closes the old channel
before bounded alternate-path recovery. Ciphertext and expired intent are never
replayed. Actual authenticated response latency is supplied to the route policy;
the adapter does not claim an unmeasured path is faster.

The firmware deliberately has a strict three-session pool and one owner slot.
If a lost owner's disconnect does not reach the gateway, opening an alternative
path may be rejected as busy until the old slot's **60-second idle deadline**.
No unauthenticated client is allowed to evict that slot. This is bounded recovery,
not a claim of seamless or instantaneous failover.

For each light change the adapter reads the affected revisions first, then reads
the room lease last. Requests carry the original 32-hex command UUID, all eight
revision positions, current boot/policy/sequence and a 1,500 ms monotonic expiry.
The firmware also enforces its original transport lease. A lost reply is resolved
by querying the same UUID after fresh authentication; no replacement write or new
UUID is generated. Conflicts require a fresh user decision.

Queued/sent receipts remain unconfirmed. Confirmation requires the matching
receipt's verified bit, a non-pending current observation, and an exact match to
the requested setting. Later changes are shown as later changes. Reusing a
completed UUID returns a current snapshot rather than restoring an obsolete
success screen. Unknown power remains unknown. No device name, guest activity,
freeze expiry or hardware signature enforcement is invented from absent fields.
WiZ observations retain their reported `unverified_lan` trust label.

Storage recovery blocks room controls while keeping available state readable.
Typed missing-key/signing-clock errors block the affected light. Gateway signing
clock readiness is not represented as measured bulb-clock or signature enforcement.

## Reproducible host checks

```sh
bash lights/security/scripts/bootstrap-test-env.sh
bash lights/security/scripts/check-gateway-wire.sh
```

The first command creates an ignored local Python environment using exact test
dependency versions. Alternatively set `LIGHTS_TEST_PYTHON` to an absolute Python
path with those versions. The second checks the pinned IDF revision, builds the
optimized native runner and executes UI/transport checks plus encrypted adapter
checks. Full Xcode is not required. SwiftPM may fetch the pinned dependency during
the build; the running fixture communicates solely through local process pipes.

The fixture uses real native SRP, P-256 verification and AES-GCM against a scripted
Python peer with public test credentials. Its application responses exercise the
actual gateway schemas, but **it is not the C service, an ESP32, a relay connection,
a BLE connection, a WiZ bulb, or proof of physical lighting behavior**. Hardware
memory/coexistence, radio conditions, commissioning and signed-bulb behavior remain
separate qualification requirements.
