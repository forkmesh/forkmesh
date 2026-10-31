# Cloudflare Workers self-host

The hosted service does not run on Workers: app.forkmesh.com and
world.forkmesh.com run on three self-hosted core nodes
([architecture](../architecture.md), [PLAN.md](../../../docs/PLAN.md)). Only the
static site forkmesh.com is the `forkmesh-www` Worker. The default self-host is a
VPS ([self-hosting](../self-hosting.md),
[start your own instance](../start-your-own-instance.md)). This page covers the
optional Workers self-host that `app/wrangler.toml` and
[the bootstrapper](automation-and-cloudflare-bootstrap.md) still support.

| Directory | Responsibility |
|---|---|
| `app/` | Product UI, `/api/*`, health and release identity, public snapshots, WebSockets, Git Smart HTTP, ActivityPub, D1, KV, AI, cron, migrations, Durable Objects |
| `www/` | Landing pages, documentation, blog, shared browser assets |
| `world/` | World shell, modules, models, audio, avatars |

The App owns the whole control plane; there is no separate API Worker.

## Routing

- The App serves the root redirect, `/health`, `/api/version` and every
  protocol route. Identity responses are `no-store`.
- Five anonymous GET routes (`/api/repositories`, `/api/network/overview`,
  `/api/repo/<owner>/<repo>/mirrors`, `/api/status`, `/api/status/world`) read
  validated, fresh-only snapshot rows. Credentials, cookies, query strings,
  cache-bypass directives and non-GET methods skip snapshots; a missing or stale
  row falls back to one no-store live response; a corrupt row fails closed.
- `www` proxies `/api/*`, Git Smart HTTP, ActivityPub, webhooks and WebSocket
  upgrades to the App, and redirects product and `/world*` routes.
- `world` serves its own assets, injects `APP_ORIGIN`, and hands other routes to
  the App through a service binding.

`app/public`, `www/public` (authored docs in `www/docs`) and `world/public` own
their assets; `app/tools/build_site_assets.py` stages `app/dist`, `www/dist` and
`world/dist`.

## Scheduled maintenance

The one-minute cron rings a singleton scheduler object whose alarm runs every
due phase. When the next phase does not fit the per-invocation subrequest
budget, it saves progress and re-arms, so a busy minute is a short chain of
invocations. Phases are failure-isolated, an interrupted phase is reported
incomplete rather than replayed, and each tick writes one bounded `cron_tick`
row (read by `/api/accounts/admin-cron-report`).

## Single-Worker topology

One App at one origin: set `PUBLIC_BASE_URL`, `API_ORIGIN`, `APP_ORIGIN`,
`WWW_ORIGIN` and the allowed CORS origins to it, and `SINGLE_WORKER_SITE=true`.
The bootstrapper creates one App Worker, one D1 database, one hostname and one
route. World is optional.

## Deploying

`app/deploy.sh deploy` and `app/deploy.sh changed` skip a target whose live
fingerprint matches its inputs. `--force` (or `FORKMESH_FORCE_DEPLOY=1`) uploads
anyway, for a Worker that is wrong in a way the fingerprint cannot see;
migrations, secrets and post-deploy checks still run.

Secret publication keeps the router signing key pair already on the App;
rotation publishes a validated pair atomically. Migrations only roll forward, so
an application rollback must accept the newest schema.
