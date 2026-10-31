# Cutover tools and switches

Production used to run as Cloudflare Workers with D1. That stack is retired
and deleted: `app.forkmesh.com` and `world.forkmesh.com` now resolve straight
to the self-hosted cores ([architecture](../www/docs/architecture.md),
[PLAN.md](PLAN.md), [OPERATIONS.md](OPERATIONS.md)). This page lists what
the move left behind that code still reads. Developer index:
[README.md](README.md).

## Switches the app still reads

`FORKMESH_READ_ONLY` and `FORKMESH_DECOMMISSIONED` (`1`, `true`, `yes`, `on`),
in `app/src/entry.py`, tested by `app/tests/test_maintenance_switches.py`.

- **Read-only.** GET/HEAD/OPTIONS and Git clone/fetch work. Every other
  request, Git push and WebSocket upgrade gets `503` with `Retry-After: 120`
  and the expected-degraded marker. Writes made by read handlers are skipped,
  and a guard in every database helper rejects write SQL as a backstop. Cron,
  queue consumers, alarms and the watchdog stop. This is an operator switch;
  the cores' own quorum-loss read-only mode is in [PLAN.md](PLAN.md).
- **Decommissioned.** Implies read-only; every request answers `503
  {"error":"decommissioned"}` and alarms delete themselves.
  `app/tools/decommission_wrangler_toml.py` writes a config with no routes,
  crons or queue consumer and both switches pinned.

## The www Worker

`forkmesh.com` stays on the `forkmesh-www` Worker. It reaches the app at
`APP_ORIGIN` with the signed `x-forkmesh-edge-client` header
([deploy/core/edge_client.md](../deploy/core/edge_client.md)) and retries
reads on `APP_FALLBACK_ORIGINS`. It deploys by hand from main with
`www/wrangler.production.toml` ([www/README.md](www/README.md)). Never deploy
the stock `app/wrangler.toml` or `world/wrangler.toml`: their custom-domain
routes would try to re-claim the production names. `.forkmesh/deploy.yml` is
manual-only and exits early: a workflow that deploys must not run on push.

## Tools

Every tool dry-runs without `--execute`, prints no secret and never takes a
secret on argv. Tests: `deploy/tests/test_cutover.py`.

| Tool | Commands |
|---|---|
| `tools/cutover_data.py` | `export`, `sqlite`, `carry` (rows only the new stack owns), `verify` (every account by name and e-mail, every node by name), `login-probe` (runs the app locally and logs in over HTTP), `rehearse` (whole pipeline with timings) |
| `tools/cutover_tls.py` | `issue` (ACME DNS-01, TXT records limited to the two `_acme-challenge` names), `push` (install pairs on every core over SSH stdin), `status`; `FMNEXT_CUTOVER_TLS_DIR` overrides the folder |
| `tools/cutover_dns.py` | `plan`, `flip`, `rollback`, `soa-min-ttl`; only the two production app names, A content must be a core address |
| `tools/cutover_identity.py` | `plan`, `apply`, `rollback`, `put-www-secret`, `install-data-key`, `restore-data-key` |
| `tools/cutover_rehost.py` | `--check`, `--apply --root DIR` (refuses a `main` checkout; only on a release worktree), `--compare` |
| `deploy/core/render.py` | optional `[production]` table in `fleet.toml`: production host names, certsync names, redirects from the old `next.*` names, and `[production.vars]` rendered into `app.env` as `FORKMESH_VAR_*` (secret-looking names are refused) |

`next_fleet install nN --steps daemon` re-pins the gateway's
`routerPublicKey`, `dashboard.canonicalOrigin` and `dashboard.assetsRoot`;
run it on every core whenever the served app host changes.

Exports of the deleted Worker data and settings live in the operator config
folder; never erase them (`AGENTS.md`).
