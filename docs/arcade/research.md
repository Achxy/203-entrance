# 203 Arcade: research and decisions (2026-11-11)

## Connectivity

The user requires ordinary browsers on Android and iPhone, without installing a
special browser. Direct Web Bluetooth cannot satisfy this. WebKit's current
[standards position](https://github.com/WebKit/standards-positions/issues/570)
opposes it, and the [implementation matrix](https://github.com/whatwg/bluetooth/blob/main/implementation-status.md)
documents the uneven support. Browser Bluetooth is therefore not the connection
route advertised by this project.

The user explicitly chose an Internet intermediary after confirming campus
peer isolation. Both the ESP and phones open outbound encrypted WebSockets to
one Cloudflare Durable Object. Phones can use mobile data independently of the
ESP's campus uplink. The user authorized registering this ESP's MAC with the
campus portal. No portal password is distributed to visitors or embedded in code.

Cloudflare's [WebSocket hibernation guidance](https://developers.cloudflare.com/durable-objects/best-practices/websockets/)
explains socket attachments and resuming a Durable Object without retaining a
JavaScript heap. Its [pricing documentation](https://developers.cloudflare.com/durable-objects/platform/pricing/)
includes SQLite Durable Objects on Workers Free. Free limits still apply;
capacity exhaustion must fail closed and leave offline management usable.
The [WebSocket runtime](https://developers.cloudflare.com/workers/runtime-apis/websockets/)
does not offer a server-side bufferedAmount API, so application credits and
message bounds are necessary. They do not prove immunity to volumetric DoS.

ESP firmware pins [esp_websocket_client 1.8.0](https://components.espressif.com/components/espressif/esp_websocket_client/versions/1.8.0/readme).
The exact source at [70bf122f](https://github.com/espressif/esp-protocols/tree/70bf122fc8bc74622dcf7e15233b5b2af9088df5/components/esp_websocket_client)
was inspected for fragmentation, retained configuration pointers, event callback
lifetime, redirect behavior, TLS errors and client destruction. Stop/destroy is
performed by the supervisor, never from a WebSocket event callback. Hostname
storage remains valid until destruction. TLS memory is released before OTA and
release-manifest checks; hardware testing is required to establish heap margin.

Campus SSIDs can advertise several access points. ESP-IDF's
[station connection scenarios](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/wifi-driver/station-scenarios.html)
describe scanning and trying matching APs. RC9 uses an all-channel scan, sorts
by signal and permits two attempts per AP. The installed board then obtained
campus DHCP on the original SSID, but the encrypted relay still timed out.
Association and a valid clock are not sufficient evidence of unrestricted
Internet access. The recorded ESP-TLS code `0x8006` means connection timeout;
it does not identify a bad certificate or establish whether the captive
portal, network filtering, packet loss or the connection deadline is responsible.

If the campus uplink fails, the ESP keeps hosting **203 IoTitty**. The fallback
controller is local HTTP reached by a top-level navigation, not insecure fetch
from an HTTPS page. See [mixed content](https://developer.mozilla.org/en-US/docs/Web/Security/Defenses/Mixed_content)
and [Chrome Local Network Access](https://developer.chrome.com/blog/local-network-access).
[Apple's Without Internet flow](https://support.apple.com/en-au/102554) retains
association. Do not intercept HTTPS or spoof campus sign-in. All local assets
are bundled; wake lock, vibration, clipboard and fullscreen are enhancements.

ESP32-C3 has one AP interface, not independent guest and administrator SSIDs. A guest radio network does not grant administrative API access. Preserve the existing HTTPS/password/CSRF/signature checks and a physical maintenance path. Limit associated stations, sockets, bodies, player slots, command frequency, and logging separately. Application limits do not make a public radio immune to RF jamming, deauthentication, or identity rotation.

## Sessions and games

The firmware owns the clock, roles, scores, menu, readiness, and physics. Clients send bounded inputs, never authoritative state. One player selects games; a second human can join only in the menu. Two-player Pong requires both ready and has no AI. One-player Pong uses the house AI. Tetris and Pac-Man are single-player. Disconnects, hidden tabs, stale inputs, inactive players, OTA and hard turn limits must release the stage to Neko. Heartbeats are liveness, not activity. Every turn has a hard limit even when a bot manufactures inputs; a cooldown gives others a window.

Tetris reference: [official controls and scoring](https://play.tetris.com/games/). Use a 10×20 well, seven-piece bag, rotation, soft/hard drop, preview, ghost projection, increasing gravity and animated line clears. Compact layout must preserve square cells rather than stretch the field.

Pac-Man reference: [Namco user guide](https://www.namcogames.com/_guides/User_Guide_-_PAC-MAN_Plus_by_Namco.pdf). Use a deliberately compact connected maze, pellets, energizers, four ghosts, buffered turns, tunnels, lives and round progression. Original rendering and compact level geometry are adaptations for 160×128, not a claim of an exact arcade emulation.

Pong house AI: deterministic reflection-based interception with bounded reaction delay and smooth movement. The ball speed bound must allow the AI to reach every predicted interception. Human speed and maximum return angle establish a finite finishing bound; make the unbeatable mode explicit. Do not teleport paddles or forge scores. Verify the bounds through simulated adversarial returns. Two-human physics remains symmetric.

## Identity and privacy

[Clerk Google social connection](https://clerk.com/docs/guides/configure/auth-strategies/social-connections/google) verifies an account, not a room number. Room/name claims stay self-reported. [Clerk metadata](https://clerk.com/docs/guides/users/extending) is bounded; use Clerk for identity and a separate durable visit table linked to its user ID. Never place an unbounded visit history in user metadata. Optional Google auth needs Internet before the local-network handoff. Offline guest entry must remain available and must never be labelled verified. Do not put Google sessions, admin credentials, private names or room numbers in public logs or leaderboards.

This document records design decisions, not completed hardware validation.
