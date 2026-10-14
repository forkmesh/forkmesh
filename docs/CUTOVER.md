# Production cutover: Cloudflare Worker stack to the self-hosted stack

This page describes how production (`app.forkmesh.com`, `world.forkmesh.com`
and the app behind `forkmesh.com`) moved from a Cloudflare Python Worker with
D1 to the three self-hosted core nodes, and the tools and switches that
remain. The served stack itself is described in [PLAN.md](PLAN.md) and
[OPERATIONS.md](OPERATIONS.md).

## End state

- `app.forkmesh.com` and `world.forkmesh.com` are unproxied round-robin A
  records (TTL 60) pointing at the cores. Each core serves them through Caddy.
- `forkmesh.com` and `www.forkmesh.com` stay on one Cloudflare Worker
  (`forkmesh-www`). It reaches the app over HTTPS (`APP_UPSTREAM = "origin"`)
  with a signed edge header and falls back to the `nN.next` node origins.
- The old app and world Workers, their D1 database, KV namespaces, queue and
  Durable Objects are deleted. Their settings and data were exported to the
  operator config folder before deletion.
- The production www Worker deploys by hand from main with
  www/wrangler.production.toml:

```sh
cd www && npx --yes wrangler@4.136.2 deploy --config wrangler.production.toml \
  --var DEPLOY_FINGERPRINT:$(python3 ../app/tools/deploy_targets.py fingerprint www)
```

Wrangler 4.136 needs Node 22 or newer. `WWW_EDGE_SECRET` stays a Worker
secret across deploys. Roll back with `npx wrangler rollback --name forkmesh-www`.
`www/wrangler.toml` has no `name`, so a plain `wrangler deploy` there fails on
purpose. Never deploy the stock `app/wrangler.toml` or `world/wrangler.toml`:
their custom-domain routes would try to re-claim the production names.

## Principles

1. **No lost writes.** Production goes read-only, a final export is taken and
   loaded into the cores, logins are verified, then DNS switches.
2. **No client update.** Clients follow DNS. No client pins a TLS certificate
   or CA; the mirror router key and node keys come with the data.
3. **Rollback by DNS.** The old stack stays intact but unrouted until the new
   one has run cleanly.
4. **Dry-run first.** Every tool dry-runs without `--execute`, prints no
   secret, and never takes a secret on argv.

## Switches

### App: `FORKMESH_READ_ONLY` and `FORKMESH_DECOMMISSIONED`

Worker vars (`1`, `true`, `yes`, `on` turn them on), implemented in
`app/src/entry.py` and tested by `app/tests/test_maintenance_switches.py`.

- **Read-only.** GET/HEAD/OPTIONS and Git clone/fetch work. Every other request,
  Git push and WebSocket upgrade gets `503` with `Retry-After: 120` and the
  expected-degraded marker. Writes made by read handlers are skipped, and a
  guard in every database helper rejects write SQL as a backstop. Cron, queue
  consumers, Durable Object alarms and the watchdog stop.
- **Decommissioned.** Implies read-only; every request answers `503
  {"error":"decommissioned"}` and alarms delete themselves.
- `app/tools/decommission_wrangler_toml.py` writes a config variant with no
  routes, no crons, no queue consumer and both switches pinned.

### www: `APP_UPSTREAM`

`binding` uses a service binding; `origin` fetches `APP_ORIGIN` with the
header

```
x-forkmesh-edge-client: v1,<ip>,<country>,<unix seconds>,<base64url mac>
mac = HMAC-SHA256(WWW_EDGE_SECRET, "v1\n<ip>\n<country>\n<ts>\n<METHOD>\n<pathname>")
```

All incoming client-address headers are dropped. Reads retry each
`APP_FALLBACK_ORIGINS` entry at `<origin>/_app/<path>`; writes are sent once.
The cores verify the header with `EdgeClientVerifier`
(`app/host/forkmesh_host/request_guard.py`).

## Steps

| # | Step |
|---|---|
| 1 | Merge and test the read-only and www-origin switches. |
| 2 | Port any app migration the new stack lacks. |
| 3 | Allow the production names in the signup challenge widget. |
| 4 | Rehearse the data move on the day's export (`cutover_data.py rehearse`). |
| 5 | Build the production release from a release worktree (`cutover_rehost.py --apply`). |
| 6 | Issue and push TLS certificates for the production names by DNS-01 (`cutover_tls.py`). |
| 7 | Stage identity values in the operator `secrets.env` (`cutover_identity.py apply`). |
| 8 | Lower the zone's negative-caching TTL if the provider allows it. |
| 9 | Announce the write pause. |
| 10 | Deploy the old app Worker with `FORKMESH_READ_ONLY=1`; confirm POST 503 and GET 200. |
| 11 | Stop the core apps and operator timers; take and restore a core backup. |
| 12 | Final export, load, verify and login probe (`cutover_data.py`). |
| 13 | Load the final file through the rqlite leader; compare user counts on every core. |
| 14 | Vault sync, node files, daemon re-pin and rolling restart (`next_vault`, `next_fleet`, `next_deploy`). |
| 15 | Probe each core directly with `--resolve` for TLS, readiness and login. |
| 16 | Point the mail provider webhook at the production name. |
| 17 | DNS flip (`cutover_dns.py flip --execute`). |
| 18 | Switch the www Worker to origin mode and set its edge secret. |
| 19 | Redirect the temporary `next.*` names to production. |
| 20 | Restart mirror daemons so they drop pooled connections. |
| 21 | Decommission the Cloudflare app and world Workers: deploy the decommission variant, never the stock config, then delete the Workers once nothing depends on them. |
| 22 | Timers back on, restore the SOA TTL, delete working copies of the export. |

Post-cutover checks: login by name and e-mail, push and clone, desktop relay,
world and chat WebSockets, e-mail and push delivery, ActivityPub WebFinger,
`/status`, IDS status and the error tracker.

Rollback: before step 17, redeploy the old Worker without the read-only switch
and restore the cores from the step 11 backup. After step 17,
`cutover_dns.py rollback` restores the custom domains; writes made on the
cores must be exported back first.

## Tools

| Tool | Commands |
|---|---|
| `tools/cutover_data.py` | `export`, `sqlite`, `carry` (rows only the new stack owns), `verify` (every account by name and e-mail, every node by name), `login-probe` (runs the app locally and logs in over HTTP), `rehearse` (the whole pipeline with timings) |
| `tools/cutover_tls.py` | `issue` (ACME DNS-01, TXT records limited to the two `_acme-challenge` names), `push` (install pairs on every core over SSH stdin), `status` |
| `tools/cutover_dns.py` | `plan`, `flip`, `rollback`, `soa-min-ttl`; only the two production app names, A content must be a core address |
| `tools/cutover_identity.py` | `plan`, `apply`, `rollback`, `put-www-secret`, `install-data-key`, `restore-data-key` |
| `tools/cutover_rehost.py` | `--check`, `--apply --root DIR` (refuses a `main` checkout), `--compare` |
| `deploy/core/render.py` | optional `[production]` table: production host names, certsync names, and `[production.vars]` rendered into `app.env` as `FORKMESH_VAR_*` (secret-looking names are refused) |

`next_fleet install nN --steps daemon` re-pins the gateway's
`routerPublicKey`, `dashboard.canonicalOrigin` and `dashboard.assetsRoot`;
run it on every core whenever the served app host changes.

## Rules learned

- An Actions workflow that deploys must not run on push; `.forkmesh/deploy.yml`
  is manual-only and exits early.
- Removing a Worker's cron does not stop it while Durable Object alarms re-arm
  themselves; use the switches or delete the Worker.
- Keep exports of anything deleted in the operator config folder; never erase
  them.
