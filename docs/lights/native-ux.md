# Native lighting menulet: research and interaction specification

Research date: 16 November 2026. This is a proposed product/engineering specification, not a claim that a native app or bulb commissioning has been implemented or validated. The display, animation, game controls and their credentials remain outside this app's UI and authorization scope.

## Native foundation and compatibility

Use a SwiftUI `MenuBarExtra` with `.window` style for everyday lighting controls, plus a separate `Settings` scene and a resizable owner-management window. Apple explicitly supports richer controls in this popover-like menu-bar surface and `LSUIElement=true` for a utility without a persistent Dock icon. Keep an accessible title on the menu-bar symbol; support reopening the app from Finder if the user removes the extra. [Apple MenuBarExtra](https://developer.apple.com/documentation/swiftui/menubarextra), [window style](https://developer.apple.com/documentation/swiftui/menubarextrastyle/window)

Proposed deployment baseline: **macOS 14**, both Apple silicon and Intel. That is a product choice, not a technical requirement of WiZ. Use `SettingsLink`, Observation and normal SwiftUI controls; keep newer APIs behind availability checks. On macOS 26, let standard controls and system materials acquire the current appearance. Do not recreate Liquid Glass with custom blur/shadow layers or force the latest visual style onto older systems. This retains familiar native layout, focus and accessibility behavior. [SettingsLink](https://developer.apple.com/documentation/swiftui/settingslink), [Apple's Liquid Glass adoption guidance](https://developer.apple.com/documentation/technologyoverviews/adopting-liquid-glass)

Start with SwiftUI; adopt public `NSStatusItem`/`NSPopover`/`NSHostingController` only if real testing identifies presentation or focus behavior that the scene cannot meet. Avoid private window access, synthesized input and global Accessibility permission. No Electron, WebView dashboard or custom drawn replacement for system controls.

## Local build and distribution evidence

Read-only checks on this Mac found:

| Item | Observed result |
|---|---|
| Host | macOS 26.5.2, arm64 |
| Compiler | Apple Swift 6.3.3 |
| Active developer directory | `/Library/Developer/CommandLineTools` |
| macOS SDK | Present under CommandLineTools |
| Full Xcode / `xcodebuild` | No Xcode bundle found; `xcodebuild` unavailable |
| `notarytool`, `stapler` | Both available under CommandLineTools |
| Valid code-signing identities | 0; no Developer ID Application or Apple Development identity found |

A SwiftPM-based app can be compiled using the installed compiler and packaged into a native `.app` bundle for local development. This feasibility assessment is not a successful app build. A public release should produce and test a universal binary or explicitly labelled architecture-specific downloads. Actual Intel behavior still needs an Intel machine or appropriate CI runner.

For straightforward distribution, use Developer ID signing, Hardened Runtime, a secure timestamp, notarization and stapling; an ad-hoc local build is not the distribution artifact. The current missing signing identity is a concrete prerequisite. Never tell recipients to disable Gatekeeper or make a quarantine-removal script the installer. Notarization is a security check, not App Store review. A ZIP must contain the already-stapled app, or distribute a stapled DMG. [Apple notarization requirements](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution), [Developer ID](https://developer.apple.com/developer-id/), [custom notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow)

## Information architecture

The following sizes and timings are initial design targets to validate, not Apple-mandated measurements.

| Surface | Contents | Interaction rules |
|---|---|---|
| Menu-bar item | Monochrome template bulb symbol; accessible name “203 Lights” | No blinking, polling count or changing width; a small problem indicator only for actionable failure |
| Quick panel | Room name, compact connection status, all-on/all-off actions, shared brightness, 3–6 favorites, bulb rows, Settings and Quit | About 360 pt wide; content height constrained to the available screen; one scrolling list rather than nested scroll regions |
| Expanded bulb/group | Power, brightness, White / Color / Scenes, selected scene's supported controls | One selected detail at a time; preserve list position; stable row identities during telemetry updates |
| Settings | Connection, Access, Appearance, Launch at Login, Diagnostics | Native form sections; ordinary users see only their own access/session controls |
| Owner window | Lights and groups, guest sessions, access policy, schedules, gateway health | Resizable native sidebar/list/detail UI; pairing and key recovery live here, never in a tiny transient popover |

The quick panel must answer “which lights are on?”, “did my change work?” and “why can I not change this?” without opening Settings. More rarely used device configuration is progressively disclosed instead of being omitted in the name of compactness. Native `Settings` provides the usual separate settings-window behavior. [Apple settings interfaces](https://developer.apple.com/documentation/foundation/adding-a-settings-interface-to-your-app)

For a group with inconsistent power states, show a real mixed-state summary such as “3 of 5 on”, with explicit **All On** / **All Off** actions. Do not pretend that an averaged Boolean describes the room. SwiftUI supports collection toggles and AppKit supports a mixed checkbox state if that is useful in selection/editing views. [SwiftUI Toggle](https://developer.apple.com/documentation/swiftui/toggle), [NSButton state](https://developer.apple.com/documentation/appkit/nsbutton/state)

## Everyday controls

- **Brightness:** native horizontal `Slider`, visible percent and accessible value. Use each light's reported range; do not encode off as an unsupported zero brightness. Power is a separate action. A group with differing levels shows “Mixed” until deliberately adjusted. A detail form may add an exact numeric field and stepper. Changes preview immediately in the UI while acknowledgement is tracked separately. Apple recommends familiar minimum-to-maximum direction, exact-value alternatives when useful, and live feedback. [Slider guidance](https://developer.apple.com/design/human-interface-guidelines/sliders)
- **White:** Kelvin slider with Warm / Cool labels and a numeric value; clamp to the selected lights' common supported range. Avoid silently simulating an unsupported tunable-white channel with RGB.
- **Color:** native `ColorPicker` without opacity, plus a small set of accessible named favorites. Convert selected colors to the protocol's specified color space before sending; a Display P3 picker value must not accidentally be treated as sRGB bytes. The normal system picker supports familiar selection and saved swatches. [Color-well guidance](https://developer.apple.com/design/human-interface-guidelines/color-wells)
- **Scenes:** named, searchable options from discovered capability/model data. Show speed, brightness or secondary-color controls only when that scene supports them. Avoid auto-playing a scene merely because its row gains keyboard focus. Unknown scene IDs remain readable in diagnostics without inventing a friendly behavior.
- **Groups:** apply the intersection of member capabilities by default. For a color action, show “3 of 4 support color” and explicitly identify an excluded light; do not send invalid commands or silently change unsupported bulbs to arbitrary whites. Report partial results per bulb and offer retry only for the failed members.
- **Favorites:** separate personal quick-access ordering from shared room scenes. Guests may use shared scenes; editing room-wide definitions, timers and policy is an owner operation unless delegated deliberately.
- **Energy and advanced features:** include only when backed by the model's verified capability. Estimated energy must be labelled estimated, and absent sensors must not become fabricated readings. Keep full feature coverage in the management surface; do not make every bulb row a dashboard.

## Onboarding and identity

**Guest entry:** open app → enter room/invitation address and guest password → authenticate the uniquely pinned gateway → open controls. A display name is optional and explicitly self-asserted. Do not require Clerk/Google or ask for the campus portal password merely to operate lights. In v1, optional **Remember access on this Mac** stores both the shared guest password and this installation's individual grant in the non-synchronizing Data Protection Keychain, only after gateway binding. Fresh Security2 authentication after transport changes requires the password; token-only reconnection is not the implemented design. A revoked grant stops all writes and must never trigger automatic re-enrollment. Explicit rejoining with a still-valid shared password is possible; rotating that password is the owner's way to exclude someone who knows it. Sign-out clears remembered access.

**Owner entry:** a separate “Owner access…” action imports the owner recovery key or pairs an approved owner installation. Never embed an owner key in the distributable, app URL, QR advertisement, crash report or `UserDefaults`. Show a one-time recovery/export workflow with a clear restore procedure. The native app may request user presence when reading the most sensitive owner credential; the gateway still performs authorization. Keychain's macOS accessibility attributes require attention to the Data Protection Keychain: use `kSecUseDataProtectionKeychain=true` when relying on `kSecAttrAccessible`, keep secrets non-synchronizing unless explicitly designed otherwise, and choose the most restrictive accessibility compatible with the operation. [Keychain accessibility](https://developer.apple.com/documentation/security/restricting-keychain-item-accessibility), [macOS attribute requirements](https://developer.apple.com/documentation/security/ksecattraccessible)

**Bulb addition:** owner starts **Add Light…** in a persistent window; app explains the actual supported commissioning route for that model; gateway performs discovery. Show identifiable candidates, then an explicit short identify action, user confirmation of the physical bulb, room/name assignment, capability readback, and a final real on/off or brightness check. “Found” and “Connected to setup Wi-Fi” are not “Paired”. Never reset or reconfigure every discovered WiZ device. Resume an interrupted wizard from a verified stage rather than replaying credential writes blindly. Commissioning is gated on the separate WiZ protocol research; there is no assumed universal consumer BLE pairing command.

The intended lifecycle is standalone, including provisioning. The current native candidate implements discovery/adoption of already joined bulbs and bounded local JSON key import, with official WiZ pairing/export labeled only as a temporary development bootstrap. It does not claim app-free commissioning or that the gateway AP forwards Internet. Daily control itself has no WiZ account or WiZ server dependency.

The local key signs outgoing gateway commands; importing it does not establish that bulbs reject unsigned requests from other LAN clients. The owner chose to leave WiZ's “Allow all controls” setting unchanged. A persistent owner-only Local Verification notice explains that guest pauses and revocation govern this app/site, while clients on the bulb Wi-Fi may bypass that policy. Do not label LAN reports authenticated or imply that unsigned-client rejection has been tested. The gateway's unsigned fallback remains disabled.

**Removal:** “Remove from this room” revokes app control and removes schedules/groups referencing that device. “Factory reset bulb” is a separate, clearly destructive owner operation with explicit scope. Losing Wi-Fi reachability does not remove a bulb automatically.

## Internet and Bluetooth behavior

Use a native `CBCentralManager` for BLE, filtered by the gateway's service UUID. Stop scanning once the intended gateway is found; discover only the required service/characteristics. A remembered peripheral identity is a reconnect hint, not a substitute for an authenticated gateway identity. Apple's guidance supports targeted discovery and avoiding unnecessary continuous/duplicate scans. [CoreBluetooth scanning](https://developer.apple.com/documentation/corebluetooth/cbcentralmanager/scanforperipherals(withservices:options:)), [remote peripheral practices](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/BestPracticesForInteractingWithARemotePeripheralDevice/BestPracticesForInteractingWithARemotePeripheralDevice.html)

Default to **Automatic** transport. A small status label says “Nearby · Bluetooth”, “Internet” or “Reconnecting”; detailed latency and failures live in Connection settings. Compare recent authenticated request/acknowledgement latency and failures, not signal strength alone. Prefer a working path, use hysteresis before changing a healthy path, and suspend path switching during a slider gesture unless that path fails. A manual Internet / Bluetooth choice is an advanced diagnostic override with a clear return to Automatic.

Commands must have one operation ID and monotonic ordering across both paths. A retry after a transport handoff reuses that identity; it must not replay an old brightness or toggle after a newer owner command. UI controls express desired state, not relative toggle messages. The same role, revocation state and guest freeze apply regardless of transport. BLE encryption alone is not app authorization.

Bluetooth permission is requested in context when enabling nearby control; refusal leaves Internet control usable. Distinguish permission denied, Bluetooth off, incompatible gateway protocol, authentication failure, and out of range. CoreBluetooth exposes authorization separately from powered-on state. Include a clear `NSBluetoothAlwaysUsageDescription`; a sandboxed app also needs the Bluetooth entitlement. [Authorization](https://developer.apple.com/documentation/corebluetooth/cbmanagerauthorization), [Bluetooth purpose string](https://developer.apple.com/documentation/bundleresources/information-property-list/nsbluetoothalwaysusagedescription), [Bluetooth entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.device.bluetooth)

The app should not change the laptop's Wi-Fi or depend on campus peer access. The gateway speaks bulb UDP on its private network. Public HTTPS/WebSocket plus BLE does not itself require scanning the Mac's LAN. If a later optional direct-LAN mode is added, macOS 15+ local-network privacy applies; permission must be requested in that feature's context. Test this using the actual signed app, not only a command-line process, because the latter can receive different treatment. [Apple local-network privacy](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)

For a sandboxed Internet/BLE-only app, start with outgoing network client and Bluetooth access; no inbound listener, USB, microphone, location, Apple Events or Accessibility entitlement is inherent to this design. Apple notes that UDP receive behavior has extra entitlement implications, another reason to keep bulb UDP on the gateway. [Network client entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.network.client)

## Fluid controls without misleading state

Bounded command behavior for protocol v1: update the dragged value on every UI event; coalesce outgoing changes to no more than two per second per active client, keeping the final desired value when editing ends. Refresh no more than once per five seconds and pause refresh while a write is pending. This fits the relay's aggregate guest budget better than a ten-per-second slider stream; actual reported gateway limits and hardware measurements remain authoritative. Keep one latest pending desired value per light/field, not an unbounded queue of every slider position. Never persist transient drags for later replay after reconnect, and do not hold scarce gateway sessions with perpetual background polling.

Maintain separate **reported**, **desired**, and **pending** state. A subtle pending indicator means accepted locally but not yet confirmed by the bulb. A transport acknowledgement alone is not proof of a light change. Hold the user's thumb during an active drag; reconcile afterwards with authoritative state. If a newer owner revision wins, end the guest's gesture and explain the owner hold rather than causing a tug-of-war or quietly continuing to send.

Use short native transitions for expanding detail, selecting a light and reconciling state. No spring on the brightness thumb, bouncing connection status, animated wallpaper or endlessly pulsing bulb rows. Respect Reduce Motion; the controls should remain equally usable with all decorative animation disabled. [Reduce Motion environment](https://developer.apple.com/documentation/swiftui/environmentvalues/accessibilityreducemotion)

| Condition | Visible behavior | Command behavior |
|---|---|---|
| Current, authorized state | Normal controls, quiet connection label | Coalesced writes; authoritative state reconciles |
| Path switching | Brief “Reconnecting” status; preserve selection | Same operation identity; no duplicate mutations |
| Bulb unreachable, gateway reachable | Individual row says “Not responding” and last-seen age | Other lights remain usable; expired command is not queued indefinitely |
| Gateway unreachable | Last-known values marked stale, reconnect action | Do not imply success or silently queue future room changes |
| Guest frozen | “Owner has paused guest controls until …” | Read-only guest controls; explicit local policy prevents bypass over BLE |
| Session revoked | Sign-in view, reason without secrets | Remove the individual grant, stop all writes, and never auto-enroll; explain explicit rejoin versus password rotation |
| Owner editing / holding | Guest sees owner hold and expiry | Reject superseded guest edits; never retry rejected changes automatically |
| Partial group operation | “3 changed; desk lamp did not respond” | Retry failed targets only after fresh readback |
| Unsupported feature | Capability-specific explanation in details | No guessed commands; shared controls omit unsupported fields |
| Unauthorized / rate-limited | Clear in-panel message; retry time if known | Respect cooldown; no automatic password hammering |

## Owner controls and conflict policy

Keep an owner-only compact **Pause Guest Controls** action in the quick panel, opening a duration/scope popover. Default to a short explicit duration, with **Resume Now** always visible. Persistent indefinite freezes need a deliberate choice and an enduring owner indicator. Show the scope (whole room or selected group) and expiry using the room's configured timezone.

In the owner window, the session table shows self-asserted device/name, creation and last-active times, access expiry, and **Revoke** / **Revoke All Guests**. Password rotation always revokes existing guest grants in v1; communicate that effect before applying it and do not offer an unsupported retain-sessions option. Shared passwords do not establish real-world identity. The UI must not call an unverified display name a verified guest.

Owner changes supersede guest choices in the gateway's arbiter. An optional **Keep My Settings** owner hold has explicit scope and duration; closing the Mac does not silently remove a server/gateway-held policy. Avoid long exclusive locks for guests: brief per-operation ordering plus coalescing is sufficient for lamp control. Audit records record who/session, target, action, outcome and time without storing passwords, keys or unnecessarily detailed personal information.

Schedules and rhythms run in the gateway or a deliberately selected durable service, not in a hidden Mac window. Show the next occurrence and timezone, distinguish bulb-local routines from gateway routines, and define daylight-saving changes and conflicts with owner holds. The UI must not offer a schedule that cannot execute after the laptop sleeps. Native Launch at Login is optional through `SMAppService.mainApp`; it is not a substitute for durable lighting automation. [Service Management](https://developer.apple.com/documentation/servicemanagement/smappservice)

## Accessibility and acceptance

Use system typography/colors and native control sizes. Include a named accessible value for every slider, readable text alongside status colors, distinct keyboard focus, and tooltips for icon-only actions. Provide full keyboard access for room selection, favorite application and owner operations. Test VoiceOver, increased contrast, reduced transparency, larger text, dark/light appearance, long names, right-to-left text, reduced motion, multiple screens, menu-bar auto-hide and screen-notch placement. Apple recommends macOS's standard 13 pt body text and sufficiently sized controls; compactness should come from progressive disclosure rather than tiny type. [Apple accessibility guidance](https://developer.apple.com/design/human-interface-guidelines/accessibility)

Required interaction tests include: popover opening/closing during an edit; native color panel focus; fifty bulbs with long names; mixed groups; concurrent guests; owner override mid-drag; session revoke during BLE control; password rotation while offline; sleep/wake; Bluetooth denial/off; captive Internet outage; duplicate/out-of-order acknowledgements; repeated transport flaps; partial bulb failures; time change; and an interrupted pairing wizard. Preserve user selection and focus during live updates. Keep diagnostics export bounded and redact every credential.

Native UI source can be implemented independently once the transport/data model is fixed. Shipping claims must remain separate: a compiled local menulet, signed/notarized download, live Internet control, live BLE fallback, successful bulb onboarding, per-model feature coverage and noninterference with the display each require their own evidence.
