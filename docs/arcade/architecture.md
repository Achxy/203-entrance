# Room 203 arcade

The public entry point is `https://203.at.achyuth.dev`. The ESP32-C3 owns the
simulation, display, player roles, clocks, admission, and deadlines. A phone is
only a controller. The 160×128 ST7735R uses the existing SPI wiring and landscape
orientation. When no game session is active, the signed application's Neko loop
and the fixed red room-number header resume.

## Connection and first use

Browser Bluetooth is unavailable in ordinary iPhone Safari and Firefox. A
browser-independent Bluetooth transport cannot be implemented by a website.
The primary route uses the public HTTPS controller and an outbound encrypted
WebSocket from the ESP to a Cloudflare Durable Object. Phones can stay on their
normal Wi-Fi or mobile data. Campus peer isolation does not prevent this route:
neither phone needs an inbound connection to the ESP. Campus Internet/captive
portal outages still interrupt online play.

The fallback is direct local Wi-Fi: join **203 IoTitty** with **neko-203-play**,
keep the connection without Internet, and open `http://192.168.4.1/play`.
The controller also works when opened directly without visiting the public site.
Every local controller asset is bundled in the signed application.

Both routes ask the physical display for a six-digit, one-use code. Enter it,
name and room (both required for guests), and consent to the private visitor
log. Google names/rooms are optional; rooms remain self-reported. A Google pass
attests to account ownership when issued, not the account's later status.

The relay accepts only five game operations: status, knock, join, command and
receipt. It has no administrator, Wi-Fi, OTA, display-configuration, or shell
operation. The board authenticates a fresh 256-bit challenge using its existing
P-256 device key; the signed message includes device, connection and boot IDs.
The Worker stores only its public key. Both legs validate TLS normally. The
board rejects redirects without forwarding its admission proof to another
destination. No campus password,
Clerk secret, Cloudflare token or GitHub token is stored in the firmware.

A hibernating Durable Object retains bounded socket attachments. The ESP emits
a clock lease every 250 ms and an anonymous public snapshot once per second.
Inputs must use a recent lease and current boot ID; commands are never queued
for later delivery. The browser allows two in-flight commands, expires queued
inputs after 300 ms, and clears held controls when its connection disappears.
The board applies the same session and physics rules to both transports.

Guest Wi-Fi pauses the saved station configuration when the relay is disabled.
Enabling the relay permits AP+STA, retaining local management if the uplink
fails. The owner configures the relay through pinned HTTPS. Changes are deferred
and serialized with OTA. The relay releases its TLS allocation before OTA or
release checking starts. Background release checks wait while a game is active.
BOOT maintenance and USB recovery remain available. The public guest password
never grants administrative access.

The relay resolves at most four IPv4 addresses through the pinned SDK's
bounds-checked multi-address resolver. It tries one address per existing retry
and backoff interval, advances after failure, and prefers the last successful
address while DNS still advertises it. Answer reordering and duplicate records
do not pin retries to a failed endpoint. No service IP is hard-coded. DNS
resolution occurs before taking the OTA lease; the supervisor rechecks the
configuration, connectivity and OTA permission afterward.

`relay_transport.c` owns an external SDK TLS/WebSocket transport pair. Its
linker wrapper changes only the dial address of that exact TLS handle. The
original hostname remains both the TLS SNI/verification name and HTTP Host;
certificate-bundle validation stays enabled. Other transports pass through
unchanged. Each connection attempt retains its 5-second transport timeout.
Redirect results are rejected before the WebSocket client can follow them.
The transport pair is destroyed only after the client task has stopped.

## Games and simulation

`arcade_core.c` is a transport-independent, allocation-free C simulation. A
20 ms step and bounded catch-up keep network work from producing unlimited
simulation work. Rendering uses a 10 KiB packed 4-bit framebuffer and a 16-color
palette. The display task converts it into the existing RGB565 output path.

- **Tetris:** 10×22 cells (two hidden rows), seven-piece shuffled bags, hold once
  per piece, ghost landing, hard/soft drop, four rotations with bounded kicks,
  500 ms lock delay with 15 move resets, line-clear animation, score and level.
  This is a compact adaptation, not a claim of official Guideline/SRS compliance.
- **Pong:** one controller uses explicit house mode. Its paddle has reaction
  delay and bounded movement, followed after a rally by a return outside the
  legal human paddle's reachable interval. This mode is deliberately unbeatable,
  disclosed on both controllers/site. Two controllers use identical human
  movement rules, no house AI, and first-to-five scoring.
- **Pac-Man:** a connected 25×19 maze rendered at five pixels per cell, four
  distinct ghost targeting policies, scatter/chase phases, tunnels, energizers,
  three lives, pellet score and repeating levels. Artwork and code are original
  compact adaptations, not a shipped commercial ROM.

Add future games to the enum, fixed union state, start/tick/render functions,
and controller-specific controls. Keep the shared admission and timeout policy.

## Shared stage and abuse limits

The first player receives the menu. A second player can join only while the
menu is open; this selects human-versus-human Pong and clears readiness. Both
must tap Ready. A joining player cannot interrupt a running match. If one
controller leaves, the survivor is promoted to player one and returns to the
menu, with the original turn deadline retained.

Device clocks enforce a five-minute maximum turn, a 25-second menu inactivity
limit, 45-second game inactivity limit, and 10-second connection deadline.
Polling/heartbeats do not count as activity. Input expires after 300 ms without a
fresh input packet, preventing a stuck paddle/key. A 10-second cooldown follows
an empty stage. Repeated menu commands cannot refresh an idle opponent. Session
commands use random 128-bit tokens and strictly increasing sequence numbers.

The six-digit code expires after 60 seconds and after successful admission, is never returned by the public
API, and is visible only on the physical display (or owner-authenticated API).
There are at most 15 failed guesses per minute globally, five per 10-second IP
window, eight IP buckets, 260 requests per 10 seconds per local IP or authenticated remote grant, four HTTP sockets,
1 KiB request bodies, and bounded socket timeouts. The owner can release a turn.

The relay permits six phone sockets, two pending device authentications, two
outstanding requests per phone, four requests outstanding across the ESP, and
only one outstanding anonymous request. Anonymous status uses cached snapshots.
Phone and board connection budgets are separate so campus NAT cannot make
phone reconnects consume the board's recovery allowance. Anonymous sockets
expire after 60 seconds, with 12-second silence and device-offline deadlines.
The board's own five-minute turn deadline remains authoritative across resumes.

These limits bound a single admission and ordinary request abuse. They cannot
prevent RF jamming, Wi-Fi deauthentication, or a nearby person repeatedly
rejoining with new identities after the cooldown. Shared Wi-Fi and HTTP are not
private from a technically capable nearby peer: transmit only game controls and
short-lived guest/identity passes here. Never send admin credentials to port 80.
The physical code establishes proximity, not a verified person's identity.

## Identity and visitor records

The public server verifies Clerk session JWTs, issuer, authorized party, and a
Google-linked account via Clerk before issuing an ECDSA P-256 `E1` pass. Passes
have an eight-hour lifetime. The ESP verifies the signature with the embedded
public key and checks expiry when it has trustworthy time. Without valid time,
a signed pass proves only identity at issuance; this limitation is reflected in
the log's `google_account_pass` label. `G1` guests are always unverified, even if
they supply fields claiming verification.

On successful admission the device signs an `R1` visit receipt with its
provisioned device key, saves it in an eight-entry NVS ring, and returns it to the
phone. The ring is bounded and reports overwrites. An owner can export it via
HTTPS. The online controller syncs the receipt immediately and retains a retry copy.
The local controller offers an HTTPS return link once Internet is restored; merely opening the public website never creates a guest visit.

The cloud verifies the device signature and embedded identity pass before
writing a visit to D1. The same visit ID and receipt hash make retries idempotent.
A lease prevents concurrent Clerk syncs. Each guest gets an identifier-free
Clerk record with unique `external_id=203-guest-<visit>` and private metadata;
there is no email, phone, password or external identity that can sign in to that
record. Lost create responses are recovered through the unique external ID.
Google visits update private metadata on their real Clerk account. Do not enable
email/phone/username sign-in for placeholder guest records.

`/log` is restricted server-side to the configured owner's verified email. It
shows at most 100 recent visits and retries pending Clerk syncs. Each refresh
removes at most three expired visits. Records older than 90 days are hidden
immediately; actual D1/Clerk deletion occurs during owner maintenance. Google
accounts themselves are retained separately. There is no claim of unattended
90-day deletion without a maintenance visit. The cloud caps new receipts at
240/day, stored visits at 2,000, and applies per-IP limits.

## Secrets and deployment

Production firmware RSA signing stays local. The separate arcade pass key is
in ignored `.local/arcade/pass-key.json`, mode 0600, and in the hosting service's
secret environment. Back it up encrypted with the device and firmware signing
materials. Its public key is `keys/arcade-pass-public.hex`. Changing it requires
a signed firmware update; retire old cloud passes at the same time.

Clerk production configuration is ignored `.local/arcade/clerk-production.env`.
Set hosting environment variables through Sites, never in source or the public
hosting manifest: `CLERK_PUBLISHABLE_KEY`, `CLERK_SECRET_KEY`, `ARCADE_PASS_KEY`,
`DEVICE_PUBLIC_KEY`, `RATE_LIMIT_SALT`, `ARCADE_OWNER_EMAIL`, and
`ARCADE_DEVICE_READY`, and `ARCADE_RELAY_URL`. Public Clerk configuration, the
relay endpoint, and the readiness bit are returned by `/api/config`. Tests use disposable keys and an in-memory database.
