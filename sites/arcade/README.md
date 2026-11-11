# 203 Arcade

Public onboarding, optional Google identity, and a private guest book for the
Room 203 ESP32 display. The ESP owns game simulation and the display. Phones use an outbound encrypted
Internet relay, with a fully local controller on the ESP as an offline fallback.

Production URL: https://203.at.achyuth.dev

## Development

Use Node 24.11.0 (CI) or later supported Node, and the committed npm lockfile:

```sh
npm run install:ci
npm run dev
npm test
npx tsc --noEmit
npm run build
```

The portable preview normally listens on `http://127.0.0.1:5173`. Guest onboarding
works without Clerk configuration; Google stays visibly unavailable. Do not put
production secret values in public environment variables. `/api/config` exposes
public Clerk configuration, the relay endpoint and readiness flag.

Tests use the real receipt/pass implementations with disposable ECDSA keys and
an in-memory SQLite database. They cover origin/size limits, signature and
identity forgery, atomic budgets, idempotence, concurrent Clerk sync, lost create
responses, and safe retention. Test worker bindings exist only in `tests/`.

Framework/runtime dependencies are pinned to patched versions. See
`../../docs/arcade/dependencies.md` for the advisory review and remaining
moderate development/optional-component findings.

## Runtime configuration

The tracked `.openai/hosting.json` identifies this exact Site and its D1 binding.
Keep its `project_id`. Use the Sites source workflow to push the exact source,
build, package, save a version, and deploy. Do not deploy with production keys
embedded in the bundle.

Required secrets:

- `CLERK_SECRET_KEY`, production instance only.
- `ARCADE_PASS_KEY`, private P-256 JWK matching the firmware's public key.
- `RATE_LIMIT_SALT`, a random value for hashed request buckets.
- `ARCADE_OWNER_EMAIL`, the verified Google email allowed into `/log`.

Other runtime values: `CLERK_PUBLISHABLE_KEY`, `DEVICE_PUBLIC_KEY` (the installed
board's public SPKI PEM), `ARCADE_DEVICE_READY` (`true` only after hardware
qualification), and `ARCADE_RELAY_URL` (`wss://…/v1/player`).

Drizzle migrations in `drizzle/` are applied to hosted D1 on publication. For a
local database, build first, then apply each pending SQL file once:

```sh
node --import ./scripts/sites-env.mjs ./node_modules/wrangler/bin/wrangler.js d1 execute DB --local --config dist/server/wrangler.json --persist-to .wrangler/state --file drizzle/0000_loose_leper_queen.sql
node --import ./scripts/sites-env.mjs ./node_modules/wrangler/bin/wrangler.js d1 execute DB --local --config dist/server/wrangler.json --persist-to .wrangler/state --file drizzle/0001_sleepy_the_order.sql
```

## Identity and maintenance

Google uses Clerk with only openid/email/profile scopes. Unverified guest
records have no sign-in identifiers. `/log` verifies owner access on the server,
shows recent visits, retries pending Clerk syncs, and deletes bounded batches of
expired records. Review it periodically; expired records are hidden after 90
days but deletion is maintenance-driven. Never make the visitor database or
Clerk private metadata public.

See `../../docs/arcade/architecture.md`, `api.md`, `research.md`, and
`validation.md` for transport limitations, security, and hardware evidence.

The framework helper files are supplied by the Sites Vinext starter; preserve
its Vite/Worker integration. Manrope's OFL license is bundled with the font.
