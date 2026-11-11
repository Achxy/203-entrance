# Room 203 Internet relay

Cloudflare Workers and a SQLite Durable Object carry bounded controller
messages between phones and the ESP. Both sides connect outbound over WSS;
campus peer isolation does not require exposed device ports. The firmware owns
admission, games, scores, deadlines and visitor receipts. This relay does not
expose the administrator API, OTA, Wi-Fi settings or device credentials.

The production endpoint is
`wss://203-arcade-relay.achyuthanugraha.workers.dev/v1/player`. Deployment is not
evidence that the installed display is connected: check the current
[qualification record](../docs/arcade/validation.md).

## Build and deploy

Use Node 24.11.0 and the committed dependency lock:

```sh
npm ci --ignore-scripts
npm test
npm run check
npx wrangler deploy --dry-run
npm run deploy
```

Wrangler authentication stays in local tool storage, outside this repository.
Production signing keys are neither stored in the Worker nor required by CI.
`wrangler.jsonc` contains the public device ID, public SPKI verification key and
allowed website Origins. Replace these deliberately for a different physical
board; possession of the corresponding private device key is required to
connect. Use a distinct Worker and Durable Object for isolated integration
tests rather than impersonating the live board.

Configure the ESP through its existing pinned owner HTTPS connection:

```sh
./entrance relay --enable --relay-host 203-arcade-relay.achyuthanugraha.workers.dev
```

Run that command from the project root. It does not join or change the host's
network. On an isolated campus network, owner HTTPS may require a separate
management adapter; the game relay is not an administrator tunnel.

## Operations

`GET /v1/status` with an allowed site Origin returns current device liveness.
Keep website readiness false until actual device authentication and gameplay
have passed. Do not put profiles, receipts, tokens, names or rooms in Worker
logs. Application budgets bound ordinary abuse and preserve device capacity;
they are not protection against RF jamming or unlimited volumetric attacks.
Free-tier limits still apply and no paid plan is required by this setup.

See the [protocol](../docs/arcade/api.md),
[architecture](../docs/arcade/architecture.md) and
[research](../docs/arcade/research.md) for clock leases, rate limits, admission
proofs, disconnect handling and scoped test evidence.
