# 203 Lights for macOS

A native SwiftUI menu-bar utility targeting macOS 14 and later. This is a **development candidate**, not evidence of a live lighting deployment. Normal launch opens disconnected Room 203 onboarding with its public gateway identity included. The real adapter checks readiness, performs SRP and signed device binding, and resumes/enrolls an individual grant before exposing controls. Construction and launch do not connect automatically. An explicit preview switch loads isolated sample lights; nothing in preview reaches a bulb, relay, Bluetooth device, or Keychain.

## Build and check

Apple Command Line Tools with Swift 6 are sufficient; the source does not require a third-party UI framework or web view.

```sh
bash lights/macos/scripts/check.sh
bash lights/macos/scripts/build-app.sh release
# Optional Apple Silicon + Intel bundle:
bash lights/macos/scripts/build-app.sh release --universal
```

The default remains an arm64 bundle at `lights/macos/build/203 Lights.app`. The explicit `--universal` mode builds arm64 and x86_64 separately, verifies identical resource bundles, combines the executable with `lipo`, and writes to `lights/macos/build/universal/203 Lights.app`. It does not replace the default bundle. Both architectures target macOS 14. The installed SDK must support both targets; cross-compilation does not establish Intel runtime behavior.

Each mode also creates `203-Lights-{arm64,universal}-development.zip` and its `.zip.sha256` beside the corresponding app. The ZIP contains the app, a development-only notice, `BUILD-MANIFEST.json`, and hashes for every other regular file in `SHA256SUMS.txt`. The script verifies architecture/deployment-target records, strict code signatures for every slice, ZIP integrity, extracted code signatures, and a complete round-trip file-hash match. Dependency notices, complete third-party license texts, upstream revision notes and the SwiftProtobuf privacy resource are bundled with the app.

Packaging requires the existing clean dependency checkout matching `Package.resolved`; missing or modified dependencies stop the build. SwiftPM updates/prefetching and credential-store lookup are disabled. The script preserves the previous app in each output directory, refuses to replace a running generated executable, and does not install or launch anything. Generated artifacts and caches are ignored by Git. Signing is ad-hoc and **does not make either package a general-distribution release**: Developer ID signing, notarization, Gatekeeper testing, and Intel execution remain separate gates. Release optimization is important for SRP performance; the check script also uses `-c release`.

Launch manually after building:

```sh
open 'lights/macos/build/203 Lights.app'
open -na 'lights/macos/build/203 Lights.app' --args --preview
open -na 'lights/macos/build/203 Lights.app' --args --preview --preview-guest --preview-state frozen
```

Preview states: `ready`, `partial`, `offline`, `stale`, `frozen`. `--preview-state` alone never activates fixtures. Quit a previous instance before comparing modes. Preview is prominently labeled in the menu panel and owner window. The menulet does not open its owner window automatically on launch.

## What is implemented

- SwiftUI `MenuBarExtra(.window)` with system controls, SF Symbols, dynamic colors, light/dark/system appearance and reduced-motion support. The panel grows to its content, then uses bounded scrolling. Only nonsecret appearance/connection preferences use `UserDefaults`; previews and checks do not persist them.
- A compact room/group/light selector, power, brightness, white temperature, RGB color, scenes and effect speed. Group controls use the intersection of reported capabilities and never invent RGB support for white-only bulbs.
- Separate reported state and pending changes. Slider changes coalesce to their latest value, the queue is bounded, requests are serialized, stale revisions fail, and partial results identify the affected bulb. Offline and stale snapshots do not allow writes.
- Separate native Settings and owner windows for guest freeze, session revocation, mandatory-revocation password rotation, bulb removal, and local verification. Unsupported standalone owner holds are disabled with a reason; owner changes still use gateway precedence. The owner notice distinguishes signed outgoing commands from bulb-side access enforcement: importing a key does not prove that a bulb rejects unsigned outsiders. The current WiZ “Allow all controls” setting is left unchanged; guest pauses/revocations cover this app and the website, while other clients on the bulb Wi-Fi may bypass them. LAN reports remain unverified.
- Real owner discovery and candidate adoption over the authenticated application adapter. WiZ-supported pairing and export are explicitly labeled a temporary development bootstrap, not standalone commissioning. Daily control has no WiZ account or WiZ server dependency. The gateway AP has no Internet forwarding, which may block official-app pairing. Discovery is bounded/cancelable; a candidate requires an explicit physical-light confirmation and is adopted with `legacy:false`. Finding a candidate is not reported as a paired, verified bulb.
- A native JSON file importer for official WiZ export version `1.0`, retained only as the development bootstrap. It accepts only a bounded regular local file, reads only `udp_signing_key`, rejects malformed/duplicate JSON and unsupported versions, fetches no URLs, and never logs metadata or copies exported capabilities into control permissions. The key is sent only through the authenticated owner action; importing it is not physical verification.
- Public gateway models and a real `GatewayAdapter` selected by normal startup. `UnconfiguredTransport` remains a fail-closed fallback for an invalid bundled identity. `PreviewTransport` is selected only by `--preview`.
- A Keychain credential store using the data-protection Keychain, `WhenUnlockedThisDeviceOnly`, no iCloud synchronization, and accounts derived from the trusted gateway identity. It is called only after identity binding and explicit retention, never by preview. No credential is bundled or logged. Explicit sign-out reports failed Keychain cleanup and offers a retry instead of pretending saved access was removed.
- Optional **Remember access on this Mac** is presented with explicit retention disclosure. A later launch offers an explicit **Use Saved Access** action, without reading Keychain on startup. For v1 the secure adapter stores the guest password plus an individual grant only after unique gateway binding. Password rotation always revokes existing guest grants. Revocation must stop writes without automatic re-enrollment; a deliberate rejoin requires entering the current shared password.
- A `RelayWebSocketChannel` for the locked relay contract: `wss` only, no redirects or credential/cookie storage, text JSON, 2 KiB envelopes, 1 KiB decoded payload, exact request/connection/boot matching, one outstanding request, bounded deadlines, and no ciphertext retries. Socket acceptance and relay role hints do not grant access.
- CoreBluetooth discovery with explicit start, bounded duration/result count, service filtering, no duplicate advertisements and explicit stop. The Settings panel lets a user choose a nearby candidate as a connection hint. Construction does not request Bluetooth permission. Advertising names remain untrusted. The peer implements bounded RX-write/TX-indication framing and reads actual gateway boot/route metadata; live radio/GATT interoperability is still untested.
- `SecureGatewayPipe` uses the actual `LightsSecurity` package for SRP and signed gateway binding before application encryption is available. Both Internet and Bluetooth feed this pipe, with fresh cryptographic state on reconnection or route change.
- A pure automatic-route policy uses monotonic observation age, two consecutive failures, 30-second dwell, and a sustained latency improvement before switching. Every chosen-route change requires fresh authentication. It is tested independently and is not presented as live failover evidence. Writes coalesce at 500ms or slower; the secure pipe also spaces every RPC by at least 500ms, including handshake, binding and paginated reads. Refresh calls are gated to at most one per five seconds. Only the actual visible, key native panel schedules jittered foreground refresh; hiding it allows an in-flight encrypted read to finish, then stops polling. Offline reads back off and stop after three failures until the panel reopens or the user refreshes.

## Adapter contract

`Sources/LightsMenulet/Models.swift` defines `MenuletTransport` on the main actor:

1. `connect(AccessRequest)` authenticates and binds the gateway's trusted identity, then returns a `RoomSnapshot`. It must not convert a claimed role or a WebSocket receipt into authorization.
2. `refresh()` fetches a complete authorized snapshot. `observedAt` is the observation time, not a newly stamped copy of stale cache data.
3. `execute(GatewayCommand)` uses `operationID` for idempotence and `expectedRevision` for concurrency. It returns an authoritative `GatewayOutcome` with per-bulb observed success or failure. Do not use a WiZ UDP ACK as proof of physical state.
4. `disconnect()` stops work and discards the secure session. UI generation guards prevent late responses from restoring a signed-out session.
5. `signOut()` additionally deletes remembered access and reports cleanup errors. It is not used as a transport-failover shortcut.

The gateway application uses compact paginated reads (`room`, individual `light`, and session pages). Assemble a complete authorized snapshot with consistency checks rather than exposing half-updated pages. Map the UI revision into the gateway's per-bulb revision array and policy epoch. Before a write, read affected lights first and the final `room` clock/lease last; otherwise paged reads can consume the two-second command lifetime. A command receipt is pending; perform bounded readback before returning `confirmedIDs`. On explicit sign-out, the adapter must remove both the remembered password and the installation grant. It must never auto-enroll after a revoked grant.

`RelayWebSocketChannel` is below this contract. The concrete adapter decodes the documented bounded application schema only after `lights/security` verifies the unique gateway. On timeout, cancellation, malformed response, relay rejection, gateway reboot, or transport switch it destroys the encrypted session. An ambiguous write is reconciled using its original UUID; it is never replayed under a fresh ID. Expected idle expiry uses a fresh same-route session and the same individual grant rather than silently creating access again.

The route coordinator uses actual authenticated RPC latency, excluding local pacing and crypto, and bounded readiness checks. It produces fresh channels and respects explicit preferences and excluded failed paths. It does not promise that an unmeasured inactive path is faster; hardware qualification must verify discovery, rebinding and shared policy. No code in this package switches the Mac's Wi-Fi, changes its routes, or transfers Wi-Fi credentials to a bulb.

## Verification and remaining gates

`LightsMenuletChecks` uses a dependency-free check runner instead of XCTest and verifies disconnected startup, explicit preview selection, capabilities, stale/offline restrictions, coalescing, partial results, owner policy, duplicate operations, races, revocation, sign-out/cleanup, bounded relay/BLE identity parsing, and route hysteresis. It does not open sockets, start scans, write Keychain entries, or operate lights. The separate `--gateway-wire-checks` option uses a scripted local Python peer for actual SRP/binding/encryption/application messages; it is host integration evidence, not physical hardware evidence. See the security package's fixture bootstrap for its pinned Python dependency.

Offscreen NSHostingView images are written to ignored `.local/lights-ui/`. They cover ready, frozen, partial, long-name and disconnected states in light/dark appearance, plus the owner verification/setup details. The owner detail text and controls render, but the native navigation sidebar is blank without a real window; those four images establish only the detail layout. The first ImageRenderer attempt omitted AppKit controls and was discarded as visual evidence. Offscreen review does not establish actual keyboard, menu-bar, permission-dialog or VoiceOver behavior; native CUA attachment was unavailable during this work.

Before a user release, complete:

- Hardware verification of the implemented secure transports, Keychain behavior in the signed app, actual grants and GATT exchanges.
- Independent, app-free bulb provisioning and key establishment for the intended standalone lifecycle. Official-app pairing/export is only a temporary development path. Physical discovery/adoption, local-key import and every reported capability also need validation; the gateway AP's lack of Internet forwarding may block the temporary pairing route.
- Real policy enforcement, bulb readback, drift/conflict reconciliation, reconnect/transport-failover tests and no display interference.
- Native visual review on macOS 14 and current macOS, VoiceOver, keyboard-only operation, increased contrast, reduced motion, menu-bar dismissal and screen-edge placement. Source compilation is not evidence of those interactive checks.
- A Developer ID Application certificate, Hardened Runtime, release signature verification, notarization, stapling and Gatekeeper testing. No valid signing identities were available on this Mac during the build. No developer account was enrolled and no notarization credentials were used.

The build contains no production credentials or automatic updater. No claim of complete WiZ app parity is made by the fixtures.

## Primary references

The research and detailed UX specification are in [`docs/lights/native-ux.md`](../../docs/lights/native-ux.md).

- [MenuBarExtra](https://developer.apple.com/documentation/swiftui/menubarextra) and its [window style](https://developer.apple.com/documentation/swiftui/menubarextrastyle/window).
- [Native sliders](https://developer.apple.com/design/human-interface-guidelines/sliders), [color wells](https://developer.apple.com/design/human-interface-guidelines/color-wells), [accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility) and [adopting Liquid Glass](https://developer.apple.com/documentation/technologyoverviews/adopting-liquid-glass).
- [Keychain accessibility](https://developer.apple.com/documentation/security/ksecattraccessible) and [synchronization semantics](https://developer.apple.com/documentation/security/ksecattrsynchronizable).
- [CoreBluetooth service-filtered discovery](https://developer.apple.com/documentation/corebluetooth/cbcentralmanager/scanforperipherals(withservices:options:)) and [radio best practices](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/BestPracticesForInteractingWithARemotePeripheralDevice/BestPracticesForInteractingWithARemotePeripheralDevice.html).
- [URLSession WebSocket receive bounds](https://developer.apple.com/documentation/foundation/urlsessionwebsockettask/maximummessagesize) and [notarizing macOS software](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).
