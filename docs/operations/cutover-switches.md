# Cutover switches (old stack to ForkMesh Next)

Two App Worker vars and one www Worker var move production traffic from the
Cloudflare stack to ForkMesh Next. They are Worker vars set at deploy time
(`--var NAME:VALUE`), never secrets.

## App Worker: `FORKMESH_READ_ONLY` and `FORKMESH_DECOMMISSIONED`

The helpers live in `app/src/entry.py` next to `EXPECTED_DEGRADED_HEADERS`
(`_env_flag`, `_read_only`, `_decommissioned`, `_maintenance_gate_response`).
A value of `1`, `true`, `yes` or `on` switches a var on. A missing or JS-null
binding counts as off. They are defined before `import app_runtime`, because
`app_runtime` and `world_durable_objects` copy entry globals when they are
imported.

### Read-only (`FORKMESH_READ_ONLY=1`)

Requests:

- GET, HEAD and OPTIONS run as usual, and so does a Git clone or fetch
  (`POST .../git-upload-pack`, `GET .../info/refs?service=git-upload-pack`,
  classified by `_git_clone_http_request`).
- Everything else answers `503` with `Retry-After: 120`, body
  `{"error":"read_only","retryAfter":120}`, and the expected-degraded marker,
  so the 5xx logger skips it. That includes every other method, Git push
  (including the `service=git-receive-pack` advertisement GET), `/mcp`, login,
  and every WebSocket upgrade. Sockets carry writes, and clients retry after
  the 503.
- `POST /api/attribution/capture` (the www page-view relay) is dropped with
  `204`.
- The gate runs at the top of `AppRuntime.fetch` and again at the top of
  `_fetch_inner`, which the ingress Durable Object reaches directly.

GETs that write D1 are skipped explicitly:

- the session touch in `_account_session_lookup`;
- `capture_page_view`, `record_api_request`, `record_site_referral` and
  `record_blog_visit` in `_fetch_inner`;
- the error_log row (`_write_error_log`) and `log_durable_object_abort`. The
  Sentry event is still sent;
- `durable_object_traffic_flush`;
- presence upkeep in `network_stats`, which also deletes rows and sends
  offline pings;
- schema DDL in `_apply_schema` and the legacy-custody marker write. The
  deploy's `migrate.sh` owns the schema.

The backstop is `_d1_write_guard(env, sql)`. It raises `ReadOnlyModeError`
for INSERT/UPDATE/DELETE/REPLACE/CREATE/ALTER/DROP. Leading comments are
skipped. `... RETURNING` counts as a write, and so does a `WITH ...` CTE that
contains a write verb (string literals are ignored). It runs in `_d1_read`,
`d1_first_primary`, `d1_run`, `d1_run_batch`, `_contribution_run_batch`,
`invalidate_public_catalog_snapshots` and `_publish_public_snapshot_group`.
These are the only functions that prepare D1 statements, and
`test_only_the_guarded_helpers_prepare_d1_statements` keeps it that way.

`ReadOnlyModeError` is an ordinary `Exception`, so a best-effort writer that
already swallows failures keeps its read working. One that escapes to
`_fetch_inner` answers the same read_only 503.

Background work stops as well (see below).

### Decommissioned (`FORKMESH_DECOMMISSIONED=1`)

This is the old stack after the new one owns the traffic. It implies
read-only. Every request answers `503 {"error":"decommissioned"}` with the
expected-degraded marker, and `ForkMeshDiscordGate.fetch` refuses.

### Background work stops under either switch

Read-only is the handover window: the new stack copies the data and then
starts its own workers. Email, Discord, ActivityPub and Solana work must
never run on both stacks, and a maintenance phase would write D1 anyway. So
under either switch:

- `scheduled()` returns without ringing the CronRunner.
- `queue()` calls `batch.ackAll()` and returns. The new stack sends that mail.
- Both Durable Object alarms (`ForkMeshCronRunner`, `ForkMeshCronWatchdog`)
  delete their alarm and return. `_coordinator_alarm` re-arms itself every
  minute, so an empty Cron Trigger list alone would never stop it.
- The CronRunner kick, snapshot-republish and mirror-wake doorbells refuse.
  Ingress (ordinary HTTP) is still routed and gated by `_fetch_inner`.
- The cron watchdog's heartbeat and alarm stop, so it sends no outage or
  recovery mail.
- Every socket Durable Object (`ForkMeshRoom`, `ForkMeshNodes`,
  `ForkMeshNoteRoom`, `ForkMeshWorld`, `ForkMeshOfficeRoom`) closes a socket
  with `1012 service restart` on its next message, and clients reconnect.

Side effects while read-only: public snapshots stop refreshing. `/api/status`
serves the last row as emergency-stale, and catalog snapshots fall back to
live reads.

## Deploying the App switches

`deploy.sh` has one opt-in hook: `FORKMESH_EXTRA_VARS="NAME:VALUE,..."`
(comma or space separated) adds `--var` bindings to the App upload. When it
is unset, nothing changes.

Read-only, through the normal pipeline. The recover flag skips the
snapshot-freshness wait, which cannot pass with the cron stopped:

```
cd app && FORKMESH_EXTRA_VARS="FORKMESH_READ_ONLY:1" FORKMESH_FORCE_DEPLOY=1 \
  FORKMESH_RECOVER_UNAVAILABLE_HTTP=1 ./deploy.sh app
```

Decommission, after the DNS flip. Do not use `deploy.sh` here: its
verification would reach the new stack, and the ordinary `wrangler.toml`
re-claims `app.forkmesh.com`. `tools/decommission_wrangler_toml.py` writes a
variant config with:

- no `[[routes]]`;
- `crons = []`;
- no `[[queues.consumers]]`;
- `workers_dev = false` and `preview_urls = false`;
- both switches pinned in `[vars]`.

It keeps D1, KV, the Durable Object bindings, `[[migrations]]` and the queue
producer, and refuses to write a variant that changed any of them.

```
cd app && python3 tools/decommission_wrangler_toml.py -o wrangler.decommissioned.toml
. ./pywrangler.sh && pywrangler deploy --config wrangler.decommissioned.toml --env "" --name forkmesh-app
```

A deploy without routes does not detach an existing custom domain. Detach
`app.forkmesh.com` from `forkmesh-app` before pointing DNS at Next.
`world/wrangler.toml` claims `world.forkmesh.com` the same way, and the world
Worker still has an `APP` service binding to `forkmesh-app`.

## www Worker: `APP_UPSTREAM`

`www/worker.js` `proxyToApp` and the attribution relay reach the App in one
of two ways:

- `binding` (the default): the `APP` service binding, as before.
- `origin`: an HTTPS fetch of `APP_ORIGIN` + path. After the cutover
  `app.forkmesh.com` is served by the Next core nodes, and the binding would
  still reach the old Worker.

The binding stays declared in `www/wrangler.toml`, so rolling back is just
`APP_UPSTREAM=binding`.

Origin mode matches ForkMesh Next's www Worker byte for byte, because the
Next host verifies it (forkmesh-next `deploy/core/edge_client.md`,
`EdgeClientVerifier` in `app/host/forkmesh_host/request_guard.py`):

```
x-forkmesh-edge-client: v1,<ip>,<country>,<unix seconds>,<base64url mac>
mac = HMAC-SHA256(WWW_EDGE_SECRET, "v1\n<ip>\n<country>\n<ts>\n<METHOD>\n<pathname>")
```

- Without the `WWW_EDGE_SECRET` Worker secret, no identity is forwarded.
- Every incoming client-address header is dropped: `cf-*`, `x-forwarded-*`,
  `x-real-ip`, `true-client-ip`, `forwarded`, and a visitor's own
  `x-forkmesh-edge-client`. So are hop-by-hop headers. `Upgrade` and
  `Connection` stay, so WebSocket upgrades pass.
- Redirects are `manual`, and bodies stream through.
- Reads (GET/HEAD) have a 15 s header deadline (`APP_FETCH_TIMEOUT_MS`). On
  a failure or a 502/504 they retry each `APP_FALLBACK_ORIGINS` entry at
  `<origin>/_app/<path>`. Entries are origins without `/_app`, and the node's
  Caddy strips the prefix. The signature covers the public path.
- Writes are sent once, since a write may already have been applied when the
  connection dropped.
- The attribution relay is signed as `POST /api/attribution/capture` and
  forwards no Cf-* copies.

Switching www to origin mode (the secret must equal the Next cores'
`WWW_EDGE_SECRET`):

```
cd www
grep '^WWW_EDGE_SECRET=' ~/projects/forkmesh-next/config/secrets.env | cut -d= -f2- | tr -d '\n' \
  | npx --yes wrangler@4.136.2 secret put WWW_EDGE_SECRET --config wrangler.toml
npx --yes wrangler@4.136.2 deploy --config wrangler.toml --var APP_UPSTREAM:origin \
  --var APP_FALLBACK_ORIGINS:https://n1.next.forkmesh.com,https://n2.next.forkmesh.com,https://n3.next.forkmesh.com \
  --var DEPLOY_FINGERPRINT:$(python3 ../app/tools/deploy_targets.py fingerprint www)
```

## Tests

- `app/tests/test_maintenance_switches.py`: flags, the SQL classifier, the
  request classifier, each handler starting with its switch check, alarm
  deletion, the decommission config, and the deploy hook.
- `app/tests/_maintenance_stubs.py`: switch-off stand-ins for tests that exec
  parts of `entry.py` into a hand-built namespace.
- `www/tests/app_origin.test.mjs`: origin and binding modes.
- `www/tests/test_edge_client_crosscheck.py`: verifies headers signed by the
  production `worker.js` with Next's own verifier. It is skipped when the
  Next checkout (`FORKMESH_NEXT_SRC`) or node is absent.
