# 203 Lights · browser client

A responsive browser interface for the same authenticated lighting
gateway used by the native menu app. Android, iPhone, macOS and other browsers
use HTTPS/WSS Internet relay transport; browser Bluetooth is not required.
The public website does not grant light access. Guest and owner actions require
Security2 plus the pinned gateway identity before any application operation.

## Run and check

No third-party runtime scripts, analytics, fonts, packages or browser-storage
credentials are required.

```sh
npm test
npm run test:security
npm run check
npm run build
npm run dev
```

`npm run dev` builds and serves the actual Worker handler at
`http://127.0.0.1:8793`; restart it after source edits. `npm run dev:source`
serves source for local browser fixture checks only and does not demonstrate
production response headers.

The deterministic build keeps the separate HTML/CSS/ESM sources and embeds
their exact bytes in one `dist/server/index.js` Worker module. It exports the
required `fetch` handler and uses no external bindings, databases or network
asset fetches. `dist/.openai/hosting.json` preserves the exact registered Site
identity; only the obsolete source manifest `static` entry was removed for
this serving-layer migration. Runtime routes expose neither that metadata nor
security fixtures, tests or source tooling. No new Site is registered.

The default view is the real connection form. `?preview=1` uses an entirely local
fixture client and always displays a preview banner. Optional preview parameters
are `role=guest` and `scenario=frozen`, `partial`, or `offline`. Preview actions
never open a gateway socket or operate real bulbs. `role` query parameters have
no effect on live authentication.

## Behavior

- Whole-room, individual and multi-light controls; temporary groups explicitly
  scoped to the current tab.
- Power, brightness, tunable white, RGB, supported WiZ scenes and effect speed.
  Group features are the intersection of member capabilities.
- One secure request at a time, central 450 ms transport pacing, coalesced
  desired changes before encryption and no ciphertext retries. Background
  polling reads one light every seven seconds rather than repeatedly paging the
  fleet; it pauses while the page is hidden, editing or performing an action.
- Before a write: read current room policy, affected light revisions, then the
  final gateway clock. Commands use bounded monotonic deadlines, explicit
  revisions and stable random IDs. Accepted/queued receipts never masquerade
  as confirmed observations. Partial, unknown, superseded and rejected outcomes
  remain distinct.
- Owner freeze/resume, 30-second choice precedence, individual/all guest
  revocation and password rotation. Guest identity labels are self-reported.
- Owner-only discovery, adoption, removal and WiZ home-key import. Signed
  gateway control is the default; legacy unsigned control requires an explicit
  acknowledgement of local-network bypass limitations.
- Importing a home key signs this gateway's commands; it does not change the
  WiZ home setting or prove a bulb rejects commands from other Wi-Fi clients.
  Owner notices on Guest access and Room setup explain that freezes, revocations
  and precedence cover our app/site. With WiZ set to Allow all controls, other
  local clients can bypass those limits. Network-wide enforcement requires both
  verified-only controls and a physical rejection check. The site leaves the
  user's WiZ setting unchanged, and the gateway legacy opt-in stays off by
  default independently of that setting.
- Official WiZ pairing remains a guided external step. Its app may need
  Internet during setup; the ESP hotspot currently provides local control,
  not general Internet routing. Discovery does not claim to pair, reset or move
  a bulb.

The native/firmware application contract is
[`lights/firmware/gateway/PROTOCOL.md`](../../lights/firmware/gateway/PROTOCOL.md).
The relay contract is [`relay/LIGHTS_PROTOCOL.md`](../../relay/LIGHTS_PROTOCOL.md).
Gateway firmware, BLE coexistence, actual bulb pairing and live controls are
separately qualified; deploying this website is not evidence of those paths.

## Privacy and identity

`src/config.mjs` contains only the owner-approved public P-256 SPKI pin,
gateway ID and public relay URL. It is copied from the existing relay public
identity; no private device key, root owner key, WiZ key, password, grant or
session token is embedded. The public `/info` response is used for liveness
only, never as a replacement trust pin.

Credentials and guest-grant tokens stay in JS memory. Input fields are cleared
after use; logout and page exit destroy the session, release retained access
and clear password fields. JavaScript immutable strings cannot promise forensic
memory erasure. The application writes no credentials or session tokens to
localStorage, sessionStorage, IndexedDB, cookies, a website account or analytics.
The hosting platform may set its own security cookies; those are separate from
application access credentials. Guest reconnect resumes its existing
grant; revoked grants are never silently reenrolled.

The optional local WiZ export importer reads only a single `udp_signing_key`.
It rejects duplicate keys, multiple-home ambiguity, invalid key encoding,
oversized input and excessive structure. It uses no export URL, cloud fetch,
invitation link or other metadata. The current parser is bounded to 64 KiB,
256 JSON values and eight nesting levels. On 2026-11-16 the owning agent
inspected the official private WiZ schema 1.0 export and confirmed that its
top-level `udp_signing_key` matches this importer. The web UI agent did not read
or publish that private export, and an actual browser file-import interaction
with it remains untested. Manual 32-hex input remains available. File selection
only fills the concealed key field; the owner must
choose Import securely to send the key through the authenticated gateway.

## UI and validation

The interface uses system fonts, semantic HTML, native range/color inputs,
native modal dialogs, visible focus, reduced-motion support, explicit labels,
dark/light appearance and touch targets of at least 44 CSS pixels. Layout and
interaction choices follow [Apple control guidance](https://developer.apple.com/design/tips/)
and the [W3C native-dialog technique](https://www.w3.org/WAI/WCAG22/Techniques/html/H102).
Scene labels follow the project's pinned pywizlight `scenes.py`; only reported
supported IDs are offered.

Model/adapter tests cover exact clocks, masks, capability intersection, malformed
replies, fresh revisions, pending/partial receipts, stale rejection, lost replies,
revocation, owner boundaries, one in-flight operation, bounded polling,
disconnect during pacing, previews, export parsing and public pin consistency.
Separate security tests use exact ESP-IDF vectors and a mock WebSocket pipe.

On 2026-11-16 the owning agent observed desktop owner controls and guest pause in
Chrome 157 through CUA; a 390×844 guest/frozen layout had no horizontal overflow,
no enabled sliders and no owner controls. The same browser passed the separate
security harness's 33 assertions across three vectors without warnings/errors.
This does not claim Safari/iPhone execution, a live gateway session or actual
bulb effects. Those remain acceptance checks. The relay feature flag remains
disabled until the gateway integration is ready.

The Sites static serving path did not apply the previous `_headers` file. The
Worker handler now explicitly sends CSP (including `frame-ancestors 'none'`),
`X-Frame-Options: DENY`, `nosniff`, `no-referrer`, restricted Permissions Policy,
`Cross-Origin-Opener-Policy: same-origin` and `Cache-Control: no-store` on every
success and error response. HTML meta CSP remains an additional restriction,
not a replacement for the response header.

Worker tests import the generated artifact and exercise exact source bytes,
MIME types, GET/HEAD, unsupported methods, unknown/private paths, malformed
escapes, query-based preview routes, deterministic packaging and headers on
errors. After publishing the same Site, the hosting owner must verify the live
HTTP response headers; local handler tests alone do not prove edge deployment.
