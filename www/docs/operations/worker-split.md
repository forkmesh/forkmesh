# Cloudflare deployment boundaries

A Cloudflare deployment of ForkMesh uses three Workers. The App owns the whole
control plane; there is no separate API or edge-control Worker.

| Directory | Script | Responsibility |
|---|---|---|
| `app/` | `forkmesh-app` | Product UI, `/api/*`, health and release identity, public snapshots, WebSockets, Git Smart HTTP, ActivityPub, D1, KV, AI, Cron, migrations, Durable Objects |
| `www/` | `forkmesh-www` | Landing pages, documentation, blog, shared browser assets |
| `world/` | `forkmesh-world` | World shell, modules, models, audio, avatars |

## Routing

- The App serves the root redirect, `/health`, `/api/version`, and every
  operational protocol. Identity responses are `no-store`.
- Five anonymous GET routes (`/api/repositories`, `/api/network/overview`,
  `/api/repo/<owner>/<repo>/mirrors`, `/api/status`, `/api/status/world`) read
  validated, fresh-only snapshot rows. Credentials, cookies, query strings,
  cache-bypass directives, and non-GET methods bypass snapshots; missing or
  stale rows fall back to one no-store live response; corrupt rows fail closed.
- `www` service-proxies `/api/*`, Git Smart HTTP, ActivityPub, webhooks, and
  WebSocket upgrades to the App, and redirects product and `/world*` routes.
- `world` serves its own assets, injects `APP_ORIGIN`, and delegates non-World
  routes and shared shell assets to the App through a service binding.

`app/public`, `www/public` (authored docs in `www/docs`), and `world/public`
own their respective assets. `app/tools/build_site_assets.py` stages
`app/dist`, `www/dist`, and `world/dist`.

## Scheduled maintenance

The App's one-minute Cron handler rings a singleton scheduler object whose
alarm runs every due phase. When the next phase no longer fits the
per-invocation subrequest budget, it persists progress and re-arms itself, so a
busy minute is a short chain of invocations on one object. Phases are
failure-isolated, interrupted phases are reported incomplete rather than
replayed, and each tick writes one bounded `cron_tick` row (read by
`/api/accounts/admin-cron-report`).

## Self-hosted topology

A default self-host is one App at one origin: set `PUBLIC_BASE_URL`,
`API_ORIGIN`, `APP_ORIGIN`, `WWW_ORIGIN`, and allowed CORS origins to it, and
`SINGLE_WORKER_SITE=true`. The bootstrapper creates one App Worker, one D1
database, one hostname, and one route. World is optional.

## Deploying

`./deploy.sh deploy` and `./deploy.sh changed` skip any target whose live
fingerprint already matches its inputs. `--force` (or
`FORKMESH_FORCE_DEPLOY=1`) uploads anyway, for a live Worker that is wrong in a
way the fingerprint cannot see; migrations, secrets, and post-deploy
verification still run.

Secret publication preserves the router signing key pair already on the App;
rotation publishes a validated pair atomically. Migrations are roll-forward, so
an application rollback must accept the newest schema; otherwise roll forward.
