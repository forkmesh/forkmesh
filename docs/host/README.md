# Host: the Python app on CPython

`app/host/forkmesh_host/` runs the Python app, written for the Workers runtime
(`app/src/entry.py`, staged into `app/src_build`), unchanged on CPython under
uvicorn. It supplies what the Workers runtime provided: the `js`,
`pyodide.ffi` and `workers` modules, the `env` bindings (D1 on rqlite, KV,
Queue, Assets, AI, Durable Objects), cron and WebSockets. On each core it runs
as `forkmesh-app.service`, next to rqlite, Caddy and the Go daemon. Design:
[PLAN.md](../PLAN.md); operations: [OPERATIONS.md](../OPERATIONS.md).

## Code map

| File | Responsibility |
| --- | --- |
| `serve.py`, `__main__.py` | CLI: `serve`, `build`, `migrate`, `rss`, `ops`, `instance-bootstrap`, `instance-runner`, `instance-single-org`, `instance-repo`, `pull-resync` |
| `runtime.py` | imports `entry` and calls `Default(ctx, env)` per fetch, scheduled and queue event |
| `asgi.py`, `ws_protocol.py` | ASGI front end for HTTP and WebSocket |
| `request_guard.py` | Host allowlist, streamed body caps, `x-forkmesh-edge-client` verification |
| `request_policy.py` | `request_block_rules` enforcement (403) and per-visitor rate limit (429) |
| `request_tally.py` | per-route `/api` counts as `web-requests` journal lines |
| `env.py`, `bindings.py` | `env` from `[vars]`, env file, `FORKMESH_VAR_<NAME>` and credentials; KV, Queue, Assets, AI |
| `d1_rqlite.py`, `d1_sqlite.py` | D1-shaped `env.DB` on rqlite or local sqlite |
| `read_only.py` | read-only mode without an rqlite leader: which requests skip or refuse writes, the 503 answer ([PLAN.md](../PLAN.md#read-only-mode)) |
| `durable.py`, `durable_cluster.py`, `cluster.py` | Durable Objects: placement, mesh, replicated storage, alarms |
| `dns_reconciler.py`, `dns_rules.py` | leaderless DNS health votes (rules shared with `tools/next_dns.py`) |
| `vault.py`, `vault_crypto.py` | sealed start, unseal socket, `host_secrets` encryption ([VAULT.md](../VAULT.md)) |
| `egress.py` | outbound guard for every fetch |
| `quarantine.py`, `ids_records.py`, `ids_alerts.py` | IDS quarantine, signed records, alert delivery ([IDS.md](../IDS.md)) |
| `background.py` | keyed background refresh and single-flight loads; cache warm-up after unseal |
| `repo_fastpath.py`, `repo_ssr.py`, `repo_views.py` | public repository reads from the local gateway; server-rendered first views |
| `error_log.py` | one `ERROR … -> 500` line and traceback for host-side failures |
| `js.py`, `ffi.py`, `workers.py`, `webcrypto.py` | the JavaScript surface, `pyodide.ffi`, Workers SDK classes, WebCrypto |
| `ops.py` | one-off operator actions inside the unsealed app |
| `instance_bootstrap.py`, `instance_repo.py`, `pull_resync.py` | self-hosted instance commands: first admin, own runner, single-organization mode, hosted repository import, moving open pulls to a moved head branch |
| `autoscaler.py` | autoscaler for app replicas (`aN`) and burst runners (`bN`): leased tick, join tokens, node states from creating to removing, metrics publisher ([AUTOSCALING.md](../fleet/AUTOSCALING.md)) |
| `fleet_api.py` | `/api/fleet/` endpoints for joining nodes, release bundles and autoscale settings, status and commands |
| `d1_caller.py` | names the caller of each D1 statement in logs and the `x-fm-d1-caller` header; `D1-done` timing lines |
| `mail_events.py` | drains the core mail spool (`/var/lib/fmnext-mail-spool`) into the app every 15 s |
| `world_health.py` | answers World's `GET /health/mobile` from the files Caddy serves in `world/dist` |
| `host_metrics.py` | five-minute window of request latency, event-loop lag, load, CPU, memory and disk |
| `ssh_helper.py` | socket-activated `fmnext-ssh-helper`: key scans and SSH installs of runners and instances |
| `app/host/requirements.txt`, `RQLITE_VERSION` | pinned dependencies and rqlite version |

## Architecture

- **One process, one event loop.** One entrypoint per event, as on Workers;
  cron `* * * * *`.
- **Database.** Statements go to rqlite `/db/request`. Plain reads are local
  (`level=none` or read-only `sqlite3` on rqlite's file); reads after a write
  in the same request use `weak`; primary-first sessions are linearizable.
  Foreign keys are enforced.
- **Read-only without a leader.** A node that has had no rqlite leader for
  10 s serves reads from its own copy, skips the side writes of GET requests
  and answers other writes with a 503 "read-only while servers recover";
  `/readyz` stays 503 with `"readOnly": true`
  ([PLAN.md](../PLAN.md#read-only-mode)).
- **Durable Objects.** Rendezvous placement over live members; the cron
  coordinator and watchdog rotate each minute (hand-off 45 s in). Objects
  owned elsewhere are proxied over the mesh. Storage in `do_storage`, alarms
  in `do_alarms` delivered exactly once by the node whose conditional delete
  removed the row; dropped rotating objects are fenced.
- **Membership** is leaderless: each node upserts only its own
  `cluster_members` row every 5 s; liveness comes from heartbeats; the static
  `FORKMESH_MESH_PEERS` map is the only source of member ids and addresses. A
  stopping node marks itself `leaving` and answers 421 to forwarded requests
  for `FORKMESH_HOST_DRAIN_MS`.
- **Sealed start.** With `FORKMESH_VAULT=1` everything but `/health` and
  `/readyz` answers `503 {"error": "sealed"}` until unsealed; no cron, queues
  or objects run.
- **Front door.** Caddy terminates TLS and serves static assets; the app
  listens on loopback. `request_guard.py` rejects unknown hosts and oversized
  bodies and verifies the homepage Worker's signed visitor header
  (`deploy/core/edge_client.md`), once per request.
- **JSON body limits.** Behind the front door, `app/src/request_bodies.py`
  reads JSON bodies up to 2 MiB, or 8 MiB for security reports, repository
  imports and `POST /api/repo/<owner>/<repo>/pulls`. A pull submission
  carries its whole signed patch series inline, and a real one passes 2 MiB
  long before the git gateway's 8 MiB request cap. Only that exact path gets
  the allowance (no trailing slash, no sub-path such as `/pulls/<n>/events`).
- **Static headers without a fleet Caddy.** A single-server instance
  (`install-instance.sh`) proxies everything to the app, so the app's
  `StaticAssets` serves the static pages itself. It applies the dist's
  `_headers` rules to those responses (security headers, the Turnstile CSP of
  `/forgot-password`, cache lifetimes), the way the fleet's generated Caddy
  routes do, and never serves `_headers`, `_redirects` or `_routes.json`.
  Before this, `/forgot-password` and `/login` on an instance carried no CSP
  or frame policy and `/_headers` was downloadable. `env.ASSETS.fetch` from
  the Worker gets the plain file; the Worker sets its own headers.
- **Versioned static files cached for a year.** A static file asked for as
  `?v=<token>` alone gets `public, max-age=31536000, immutable`, but only
  when the token equals the file's own `content_version` (the first 12 hex
  digits of its SHA-256, the value the build stamps) and no `_headers` rule
  made it `no-store` or `private`. Checking the hash keeps a core still on the
  previous build, during a rolling deploy, from answering a new page's
  `?v=NEW` with the old file and having browsers and the edge keep that for a
  year. Any other token keeps the normal revalidating header.
- **Block rules and rate limit.** The app and world names point straight at
  the cores, so the cores enforce what a CDN WAF would:
  - enabled `request_block_rules` (`user_agent_keyword`, `ip`, `cidr`,
    `ip_range`) answer 403; rules reload every 60 s. A Google-named agent
    passes only with forward-confirmed reverse DNS; listed AI assistant
    agents are never keyword-blocked;
  - each visitor gets `FORKMESH_RATE_LIMIT_REQUESTS` per
    `FORKMESH_RATE_LIMIT_PERIOD_SECONDS`, then 429 for
    `FORKMESH_RATE_LIMIT_TIMEOUT_SECONDS` (in memory, per core); an IPv6
    visitor is counted by its /64 (`rate_key`), since one customer holds a
    whole /64 and could otherwise rotate through 2^64 budgets;
  - the visitor is the verified edge address, else `Cf-Connecting-Ip` set by
    Caddy; `/health`, `/readyz` and `/api/fleet/*` are exempt.
- **Request rules in Caddy.** Each rules load writes
  `FORKMESH_REQUEST_RULES_FILE` when it changes; a root path unit runs
  `deploy/core/request_rules.py`, which validates it, renders
  `/etc/forkmesh-next/caddy/request-rules.caddy` (address matcher, keyword
  matcher, and a `forward_auth` matcher to `/_host/request-policy` for
  Google-named agents) and reloads Caddy only on change. Keywords are limited
  to printable ASCII and escaped, so they cannot inject directives. The
  include is runtime state excluded from IDS integrity checks.
- **Repository fast path.** Public repository reads are answered from the
  local gateway before the app; non-200 answers fall through. It adds the
  app's CORS answer itself. `gateway_query` must send the gateway the same
  query the app's router builds (`_https_mirror_request_query`):
  `test_query_matches_the_worker_router` compares the two. When the app
  learned `history?path=` but the fast path did not, every folder's History
  tab on an instance showed the whole repository's commits, because the fast
  path answered first and dropped `path`.
- **Active content from the fast path.** Every fast-path answer (live,
  in-memory and stale) carries `Content-Security-Policy: default-src 'none';
  sandbox`, and a `raw` answer whose type is empty or contains `html`, `xml`
  or `script`, or is `multipart/*`, goes out as `application/octet-stream`
  with `Content-Disposition: attachment`. This is the rule the Worker applies
  to routed reads (`edge_routing.active_content_type`;
  `test_active_content_rule_matches_the_worker_router` keeps the two equal),
  and the gateway's `raw_spec` uses it too. Why: the fast path answers before
  the Worker and kept only the gateway's type and disposition, dropping its
  CSP. The gateway blocked only seven types, so a file such as `feed.atom`
  (`application/atom+xml` from Debian's `/etc/mime.types`) was served inline
  without a CSP. Browsers display such a file as an XML document and run its
  XHTML scripts, on the App origin.
- **Commit-pinned answers in memory re-check visibility.** A hit in the
  in-memory cache of commit-pinned reads (`ImmutableCache`), whether served
  by `handle` or returned by `read` to the view bundles, is used only while
  `repo_views.still_public` says the gateway still serves the repository on
  its public route. That answer is kept for 5 seconds
  (`PUBLIC_CHECK_SECONDS`). If the check fails, the hit falls through to the
  Worker. When the public route answers 403, 404 or 410, every cached entry
  of that repository is dropped (`forget_repository`). A 404 on any
  fast-path read also clears the 5-second answer, so the next hit checks
  again. If the gateway is unreachable, the entries are withheld but kept.
  Why: hits used to be served without any check, so after a repository went
  private, was quarantined or was deleted, its commit-pinned reads kept being
  served from memory until eviction or a restart.
- **Stale answers during a gateway restart.** The fast path keeps the last
  200 of each non-pinned `tree`, `blob`, `blobs`, `history`, `commit`,
  `branches` and `stats` read (`StaleCache`: 256 entries, 16 MB, 1 MB per
  answer, 15 minutes). When the gateway fails, the read still falls through to
  the Worker first; only when the Worker then answers 502, 503 or 504 does the
  host send the kept answer instead, as a 200 with `X-ForkMesh-Stale: 1` and
  `no-store` (`stale_answer`/`send_stale`, called from `HostASGI` after the
  Worker). The dashboard shows "Updating…" over such a page and reads it again
  (`docs/app/repo-views.md`). Only the gateway's `/v1/public/...` route fills
  the cache, so private repositories and private-view reads (`viewer`/`sig`)
  never enter it. A 404 from the gateway drops that entry and, if the
  repository still has others, re-checks it with `still_public`; a repository
  that is no longer public loses all its entries (`forget_repository` clears
  both caches), and `stale_answer` refuses any repository `repo_views` last
  saw as not public (`known_not_public`). Before this, the other entries of a
  repository made private stayed answerable for 15 minutes. When the gateway
  is down and the visibility is unknown, the 15-minute limit still bounds it. Commit-pinned reads stay in the existing
  `ImmutableCache`. This exists because every instance deploy restarted the
  gateway and a single-server instance showed pull request pages as unavailable for
  about 40 s (`docs/server/gateways.md`).
- **Egress.** Every subrequest refuses private, loopback, metadata, CGNAT and
  mesh addresses at connect time, re-checks redirects, and fails over across
  resolved addresses. Without `FORKMESH_SERVES_PRODUCTION=1`,
  `*.forkmesh.com` is refused by name. The pool speaks HTTP/1.1 and offers
  HTTP/2 only to push-notification hosts (`HTTP2_ONLY_HOSTS`).

## Interfaces

- HTTP on `127.0.0.1:8080`: `/readyz`, `/health`, `/api/version` (`BUILD_REV`).
  With `FORKMESH_HOST_DEBUG=1` (tests only) also `/_host/cluster`,
  `/_host/scheduled` and `/_host/read-only?on=1|0|auto`.
- Mesh listener on the WireGuard address, HMAC-SHA256 under
  `FORKMESH_MESH_SECRET`.
- Unseal socket `/run/forkmesh-next/unseal.sock` (0600): `unseal`, `seal`,
  `seal-restart`, `reload`, `status`, `ops`.
- `web-requests {"t":<ms>,"s":10,"r":[[group,class,count,sum_ms,max_ms],...]}`
  journal lines with identifier-free route groups.

## Configuration

Names only; values come from `deploy/core/templates/app.env` and credentials:
`FORKMESH_PLATFORM`, `FORKMESH_ENV_FILE`, `FORKMESH_VAR_<NAME>`,
`FORKMESH_HOST_DB` (`sqlite:PATH`, `rqlite:URL`), `FORKMESH_HOST_ASSETS_DIR`,
`FORKMESH_VAULT`; cluster `FORKMESH_NODE_ID`, `FORKMESH_MESH_LISTEN`,
`FORKMESH_MESH_URL`, `FORKMESH_MESH_SECRET`, `FORKMESH_MESH_PEERS`,
`FORKMESH_CLUSTER_RQLITE_URL`, `FORKMESH_RQLITE_NODE_MAP`; rate limit
variables above; `FORKMESH_REQUEST_RULES_FILE`; `CORS_ALLOWED_ORIGINS`;
`AI_API_URL` (unset means AI features degrade quietly).

## Build, run, test

```sh
python3.13 -m venv .venv && .venv/bin/pip install -r app/host/requirements.txt
cd app/host && ../../.venv/bin/python -m forkmesh_host build
cd app/host && ../../.venv/bin/python -m forkmesh_host serve --port 8080 --db sqlite:/tmp/fm.sqlite --migrate
.venv/bin/python -m pytest app/tests/host
.venv/bin/python -m pytest app/host/tests_rqlite app/host/tests_cluster app/host/tests_unit
```

`tests_rqlite` and `tests_cluster` start real rqlite processes and skip when
`rqlited` is missing (`RQLITED=/path`). `python -m forkmesh_host rss` reports
memory after importing everything.

## Operations

- Deployed by `tools/next_deploy.py` ([deploy/README.md](../deploy/README.md)).
- On a node: `journalctl -u forkmesh-app`, `fmnext-unseal --status`,
  `fmnext-app-ops` for one-off account actions,
  `journalctl -u fmnext-request-rules`.

## Security

- `DATA_KEY` exists only in the unsealed process's memory.
- The mesh accepts only members of the static map, signed with the mesh
  secret; body caps apply before authentication.
- Egress cannot reach private addresses.
- Blocked and abusive clients are refused before the app runs.
- A quarantined node is excluded from objects, cron and DNS.

## Traps

- Caddy serves static files itself: block rules reach them through the
  include, but the rate limit covers only what the app answers.
- In Caddy `remote_ip` is the TCP peer; for requests relayed by the homepage
  Worker that is a CDN address, so address rules there miss the visitor (the
  app still catches it through the signed header).
- The homepage's own static pages are protected only by the CDN WAF rules
  synced from the same table.
- Push-notification hosts accept HTTP/2 only; do not offer HTTP/2 to other
  origins, because abandoned HTTP/2 bodies can exhaust a pooled connection's
  receive window.
- Some resolvers return a wrong address for push hosts; egress failover
  across addresses and re-resolution handles it.
- Fleet nodes run Debian's Python 3.13 while the code also targets Pyodide's
  3.14.
- `serve` rebuilds `app/src_build` when sources are newer (`--no-build` skips
  it); tests that stage it must call `importlib.invalidate_caches()`.
- Adding a `*_bi` column or encrypted table requires registering it in
  `tools/next_rekey.py` and [REKEY.md](../REKEY.md), or the host tests fail.
