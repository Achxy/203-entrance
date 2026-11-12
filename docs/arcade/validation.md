# Arcade qualification checkpoint — 11–12 November 2026

**Not yet qualified as a stable Internet-connected arcade.** The public site
and relay are deployed, and the signed arcade firmware is installed. Actual
phone-to-display Internet play and a final continuous operating soak remain
open. The earlier OTA foundation results do not establish these new gates.

## Prepared room-number header

The owner's later request replaces the three rotating captions with a fixed
red `203`, using the same bitmap font at 4× integer scale. The landscape header
remains exactly rows 0–39, its yellow divider remains row 40, and the visible
red ink is centered at x=36–123, y=6–33. No game source or Neko asset changed.
All 144 C/browser raster comparisons pass. An aggregate digest captured from
the preceding firmware confirms that the divider and every pixel below it
remain byte-identical across all 24 frames and both orientations. The 68 host
tests also pass. These are local checks; the new header is **not yet installed**
on the inaccessible display.

The next firmware also reports `station_mac`, `ap_mac` and the explicitly named
`station_connected` network status field. Inspection of the installed source,
ESP-IDF's interface mapping and the original USB chip records confirms the
factory-derived, fixed station MAC policy; no application code overrides or
randomizes MAC addresses. Actual driver addresses will be visible in the next
firmware's authenticated status response. The legacy `network.online` field
means DHCP connectivity, not verified campus Internet access.

## Prepared relay failover (RC12)

The RC12 candidate retains the fixed red room number and adds bounded relay
address failover. On 12 November, two IPv4 answers for the relay hostname had
different reachability from the Mac's existing campus connection: one TCP
attempt timed out and the other completed verified TLS in approximately
0.3 seconds. This is evidence of a campus-path problem and a missing fallback
in the installed firmware; it is not proof of the ESP's current DNS answers.

All 69 host tests pass. The production transport adapter is compiled with
address/undefined-behavior sanitizers and tested against controlled DNS and
transport doubles for failure advance, success preference, reordered/expired
answers, duplicate/capped records, DNS failure, allocation cleanup, redirect
rejection, original TLS/HTTP identity and unchanged unrelated transports.
Disassembly of the built ESP image confirms that both the SDK WebSocket client
and its underlying WS transport call the scoped connect wrapper. The signed
candidate builds within the unchanged 1,966,080-byte slot and passes the
recovery/signature/configuration audit. These checks do not establish actual
radio/TLS behavior or the OTA quiescence deadline on hardware.

The owner submitted the fixed station MAC to iBus; the registered-address list
contains the correct value. The portal displays all entries without colons,
so that display format is not evidence of a registration error. Subsequent
public relay status still says offline, and a single TCP probe to the ESP's
last campus IP timed out. The laptop's Wi-Fi and network settings were not
changed. RC12 has **not been installed**. The game relay intentionally provides
no administrator or OTA tunnel; a local management route is still needed to
install the candidate. The continuous operating soak and real Internet game
qualification cannot be claimed from these local tests.

## Installed display

The second ESP32-C3, `entrance-c7f088`, confirmed signed `0.3.0-rc.9` through
the actual hotspot OTA protocol. The image is 1,314,816 bytes including
signature padding, leaving 651,264 bytes in each unchanged application slot.
The update wrote the inactive slot, rebooted, accepted fresh updater contact
and passed the healthy trial. RC8 remains a valid rollback target. No USB
reflash or eFuse change occurred.

RC9 upload took 62.76 seconds. Subsequent certificate-pinned administrator
login took 27.83 seconds including TLS. Embedded Neko readback matched the
source ECL1 byte-for-byte. The short post-installation window measured 10.56
redraws/second, including independent caption redraws. Settings and panel
configuration were retained. This is an installation check, not a one-hour
stability result. The user previously confirmed visible movement of both the
physical cat and captions; a successful SPI transaction alone cannot prove
visible output.

The guest SSID is **203 IoTitty**. A later authenticated network configuration
restored the original campus SSID. The ESP obtained a DHCP address with RSSI
approximately −57 dBm, but its outbound relay connection reported ESP-TLS
`0x8006` (connection timeout), with no successful WebSocket connection. The
public relay continued to report the display offline. Do not treat campus
association or MAC registration as an Internet-connectivity pass.

The RC8 pre-update RAM telemetry contained 29 minute samples with no unhealthy
observations. It spans configuration changes and less than an hour; it is not
a valid substitute for the final RC9 soak.

## Website, identity and relay

- `https://203.at.achyuth.dev` resolves and serves the deployed Internet-first
  site. The signed-in onboarding tells players to retain campus Wi-Fi or mobile
  data; joining the display hotspot is a collapsed fallback.
- Production Google sign-in completed through Clerk for the authorized owner.
  The site showed a verified account and obtained a signed identity pass with
  the optional room field empty. The updated OAuth homepage/privacy URLs were
  saved in Google Cloud.
- The owner-only guest book loaded successfully and showed an existing
  device-admitted unverified guest visit as saved to Clerk. Guest identity and
  room assertions remain unverified. This observation does not establish a
  new Google-verified physical admission through the Internet relay.
- The Cloudflare Worker is deployed with signed device admission, fresh
  challenge authentication, bounded frames/credits, persisted admission rate
  limits and separate device/phone budgets. Real ESP relay authentication is
  still unverified; synthetic transport tests are listed below.
- `ARCADE_DEVICE_READY` remains false. The website explicitly says the display's
  Internet connection is being checked. No live-play availability is claimed.
- Site version 3 published successfully with the dependency security update;
  the domain, access policy and runtime secret revision were retained.

## Automated evidence

All 69 host test methods pass, exercising the production OTA parsers/state machine,
settings, identity boundaries, game logic, Neko decoding and relay framing.
The relay suite has 15 passing tests for signed admission, malformed envelopes,
replays, aggregate credits, stale pulses, NAT budgets, hibernation and reconnect.
The site suite has eight passing tests for identity passes, signed receipts,
origin/body limits, atomic rate budgets, owner privacy, idempotent Clerk sync,
concurrent leases and retention. These tests use host implementations or
doubles; they do not prove radio behavior, physical-phone compatibility or
Internet latency on the installed ESP.

The website dependency review and remaining moderate advisories are recorded
in [dependencies.md](dependencies.md). Do not describe a passing build as an
absence of dependency risk.

Previous desktop/mobile-width visual checks and Neko frame/raster comparisons
are recorded in [the overall validation record](../validation.md). Physical
Android/iPhone game-controller testing is outstanding.

## Open acceptance gates

| Gate | Status |
|---|---|
| Real ESP-to-relay encrypted authentication | Blocked by the observed connection timeout |
| Internet Tetris, solo Pong, two-human Pong and Pac-Man | Requires the real device relay connection |
| Real disconnect/reconnect and hostile-client tests | Protocol tests pass; hardware cases remain |
| Google-verified physical visit and receipt sync | Web sign-in/pass and existing guest log verified; end-to-end admission remains |
| Twenty alternating updates on final combined firmware | Not completed; original-board foundation results remain separately scoped |
| GitHub download on the final combined firmware | Not completed |
| Sixty continuous minutes on the final configuration | Not completed |
| Physical power interruption on this board | Unavailable; prior board's evidence is not relabelled |
| Stable release and final recovery bundle | Pending final qualification |

The owner has prohibited switching the laptop's Wi-Fi for further tests. The
connection helpers were stopped. Further hardware qualification needs an
operational outbound route or an independent management interface that leaves
the laptop online. Do not retry automatic hotspot switching or infer approval
from the earlier test scripts.

Private raw evidence is under `.local/qualification/c7f088/reports/` and
`.local/arcade/evidence/`; it includes device credentials or personal records
and must not be published wholesale.
