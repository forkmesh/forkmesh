# Host: the Python App on CPython

`app/host/forkmesh_host/` runs the ForkMesh Python App, written as a
Cloudflare Worker (`app/src/entry.py`, staged into `app/src_build`), unchanged
on CPython under uvicorn. It supplies the pieces the Workers runtime used to
provide: the `js`, `pyodide.ffi` and `workers` modules, the `env` bindings (D1
on rqlite, KV, Queue, Assets, AI, Durable Objects), cron, and WebSockets. This
is what serves app.forkmesh.com and world.forkmesh.com. On each core node
(`fmnext-n1..3`) it runs as `forkmesh-app.service`, one process per node, next
to rqlite, Caddy and the Go daemon. The design is in [PLAN.md](../PLAN.md)
phases 1 to 3; operations are in [OPERATIONS.md](../OPERATIONS.md).

## Code map

| File | Responsibility |
| --- | --- |
| `serve.py`, `__main__.py` | CLI: `serve`, `build`, `migrate`, `rss`. |
| `runtime.py` | Imports `entry` from `app/src_build` and calls `Default(ctx, env)` for each fetch, scheduled and queue event. |
| `asgi.py` | ASGI front end: HTTP and WebSocket requests into `Default.fetch`. |
| `ws_protocol.py` | uvicorn's `websockets-sansio` protocol, except that a handshake still pending at shutdown gets an expected-degraded 503. |
| `background.py` | `env.HOST_BACKGROUND`: keyed background refreshes and shared single-flight loads for the app, and the cache warm-up after unseal. |
| `request_tally.py` | Per-route `/api` request counts for the desktop's Web Requests tab, written to the journal as one `web-requests` line per 10 seconds. |
| `request_guard.py` | Front-door checks before the Worker sees a request: Host allowlist, streamed body caps, and `x-forkmesh-edge-client` verification. |
| `request_policy.py` | The `request_block_rules` table enforced on the cores (403), and a per-visitor request rate limit (429). |
| `env.py` | Builds `env` from `app/wrangler.toml` `[vars]`, an env file, `FORKMESH_VAR_<NAME>`, and systemd credentials. |
| `bindings.py` | Non-database bindings: `LocalKV`, `LocalQueue`, `StaticAssets`, AI. |
| `d1_rqlite.py`, `d1_sqlite.py` | D1-shaped `env.DB` on rqlite (fleet) or `sqlite3` (local development). |
| `durable.py`, `durable_cluster.py`, `cluster.py` | Durable Objects: single-process host, and the cluster host with placement, the internal mesh, replicated storage and alarms. |
| `dns_reconciler.py`, `dns_rules.py` | Leaderless DNS health votes for the `app.next` / `world.next` records only (rules shared with `tools/next_dns.py`); the fleet runs it `dry` (`deploy/fleet.toml` `[dns]`). |
| `vault.py`, `vault_crypto.py` | Sealed start and the unseal socket; encryption of `host_secrets` ([VAULT.md](../VAULT.md)). |
| `egress.py` | Outbound guard for every `fetch`: public addresses only, pinned at connect; `*.forkmesh.com` refused by name unless `FORKMESH_SERVES_PRODUCTION=1`. |
| `quarantine.py`, `ids_records.py`, `ids_alerts.py` | IDS quarantine view, signed records, admin alerts ([IDS.md](../IDS.md)). |
| `repo_fastpath.py`, `repo_ssr.py`, `repo_views.py` | Public repository reads from the node's own Git gateway, with their own CORS answer; server-rendered first views; one-bundle reads for pull, issue and discussion pages ([PERF_PAGES.md](../PERF_PAGES.md)). |
| `world_health.py` | `GET /health/mobile` on the World host (`WORLD_ORIGIN`, world.forkmesh.com). |
| `js.py`, `ffi.py`, `workers.py`, `webcrypto.py` | The JavaScript surface the app imports (`from js import ...`), `pyodide.ffi`, the `workers` SDK classes, and WebCrypto on `cryptography`. |
| `ops.py` | `python -m forkmesh_host ops ...`: one-off operator actions run inside the unsealed app (`fmnext-app-ops`, `fmnext-unseal --ops`). |
| `paths.py` | Repository layout the host relies on. |
| `app/host/shims/` | `js`, `workers` and `pyodide` modules put first on `sys.path`. |
| `app/host/RQLITE_VERSION`, `app/host/RQLITE_FINDINGS.md` | Pinned rqlite version (v10.3.6) and the Phase 0 conformance findings. |
| `app/host/requirements.txt` | Pinned Python dependencies (uvicorn, httpx, h2, cryptography, orjson, pytest). |

## Architecture

- **One process, one event loop** (uvloop and httptools when installed).
  `runtime.HostRuntime` creates one entrypoint per event, as Workers does.
  The cron expression is `* * * * *`.
- **Database.** Every statement goes to rqlite's `/db/request`. Plain reads
  are served by the local node (`level=none`, or an in-process read-only
  `sqlite3` connection on rqlite's file); after a request writes, its later
  reads use `level=weak`; `withSession("first-primary")` reads are
  linearizable. A local-read miss is confirmed with the leader (0adec3d).
  Foreign keys are enforced like D1.
- **Durable Objects on a cluster.** Placement is a rendezvous hash over the
  live members; the cron coordinator and CronWatchdog rotate round-robin
  every cron minute (hand-off 45 s in). An object owned elsewhere is proxied
  to its owner's mesh listener (HTTP relayed, WebSocket frames bridged).
  `ctx.storage` lives in rqlite `do_storage`; alarms in `do_alarms`, delivered
  exactly once by the node whose conditional `DELETE` removed the row. A
  dropped rotating object is fenced. No object depends on the rqlite leader.
- **Membership** is leaderless: each node upserts only its own
  `cluster_members` row every 5 s; liveness is derived from heartbeats. The
  static map `FORKMESH_MESH_PEERS` (rendered from `deploy/fleet.toml`) is the
  only source of member ids and mesh addresses.
- **Graceful stop drains.** On shutdown a node that hosts objects writes its
  row as `leaving`, which no node counts as live, and refreshes its own view.
  Its mesh listener keeps running for `FORKMESH_HOST_DRAIN_MS` (2.5 s) and
  answers 421 to peers still forwarding to it, so they re-place the object.
  In its first 60 s a node's Durable Object requests wait up to 2 s for it to
  start hosting, so the second between unseal and the next view tick no longer
  answers "no quorum". Readiness still flips on that tick: announcing the node
  earlier made cores disagree on owners for a second (two world instances).
  `/_host/cluster` shows `leaving` in `memberStatus` and counts `drains`.
- **Sealed start.** With `FORKMESH_VAULT=1` the node boots without
  `DATA_KEY`; everything but `/health` and `/readyz` answers
  `503 {"error": "sealed"}`, cron and queues do not run, and the node hosts no
  Durable Objects until the operator unseals it.
- **Front door.** Caddy terminates TLS and serves static assets; the app
  listens on 127.0.0.1 only. `request_guard.py` rejects unknown hosts and
  oversized bodies and verifies the signed visitor header from the homepage
  Worker `forkmesh-www` (forkmesh.com), which fetches the App over the
  internet (`deploy/core/edge_client.md`).
- **Blocked requests and rate limit.** `app.forkmesh.com` and
  `world.forkmesh.com` point straight at the cores (DNS only), so
  Cloudflare's WAF rule and zone rate limit, which protect forkmesh.com, never
  see their traffic. `request_policy.py` applies the same two policies in
  `HostASGI` right after the Host allowlist, for HTTP and WebSocket handshakes:
  - Enabled `request_block_rules` rows (`user_agent_keyword`, `ip`, `cidr`,
    `ip_range`; the same table `app/sync_request_block_rules.sh` publishes to
    the WAF) answer `403 Forbidden`. The rules are read from the local rqlite
    every 60 s in the background; a failed read keeps the last rules and
    retries in 10 s. A User-Agent that matches a keyword but names Google
    passes only when its address is a verified Google crawler: reverse DNS
    ends in `googlebot.com`, `google.com` or `googleusercontent.com` and the
    name resolves back to the address (Google's documented check, the local
    stand-in for the WAF's `cf.client.bot`). Results are cached for a day.
    A User-Agent that names ChatGPT (`ChatGPT-User` or `OAI-SearchBot`,
    any case) is never blocked by a keyword rule (`WELCOME_AGENTS`); see
    "Recent changes", 2026-10-07.
    Keyword rules apply to every path except the exempt ones below, the AI
    guestbook and anonymous agent notes included
    ([../app/agent-notes.md](../app/agent-notes.md)).
  - Caddy applies the same rules to everything it serves itself, before any
    route: static files, the world index, legacy-name redirects, the per-node
    names and the private Git route. See "Request rules in Caddy" below.
  - Each visitor address gets `FORKMESH_RATE_LIMIT_REQUESTS` requests per
    `FORKMESH_RATE_LIMIT_PERIOD_SECONDS`; the next one answers
    `429 Too Many Requests` with `Retry-After` and the address stays limited
    for `FORKMESH_RATE_LIMIT_TIMEOUT_SECONDS`. The defaults are 240 per 10 s
    and 30 s, twice the forkmesh.com zone rule
    (`app/tools/sync_request_rate_limit.py`, 120 per 10 s): over the 24 hours
    before 2026-09-28 the busiest visitor prefix in the cores' access logs
    peaked at 109 requests in 10 s (a household of signed-in devices, static
    and Git files included), and verified Googlebot at 61. The count is per
    core and in memory; a restart forgets it.
  - The visitor is the signed `x-forkmesh-edge-client` address when it
    verifies, else `Cf-Connecting-Ip` (Caddy sets it to the TCP peer and drops
    any client copy). `/health`, `/readyz` and `/api/fleet/*` are exempt, and
    loopback callers with no visitor header (the deploy gate) are not rate
    limited.
- **Request rules in Caddy.** Every successful rules load also writes the
  parsed rules to `FORKMESH_REQUEST_RULES_FILE`
  (`/var/lib/forkmesh-next/app/request-rules.json`, `Rules.export`), but only
  when the content changed. On each core the root unit
  `fmnext-request-rules.path` watches that file and runs
  `fmnext-request-rules.service` (`deploy/core/request_rules.py`), which
  validates it, renders `/etc/forkmesh-next/caddy/request-rules.caddy` and runs
  `systemctl try-reload-or-restart caddy.service` only when the include
  changed. Every site block in the rendered Caddyfile (app, world, legacy
  names, `nN`) imports `request-rules*.caddy` first. The include has three
  named matchers, each skipping `/health`, `/readyz` and `/api/fleet/*`:
  - `@request_rules_address`: `remote_ip` over the `ip`, `cidr` and
    `ip_range` rows (ranges summarized into CIDRs). Caddy answers
    `403 Forbidden` itself.
  - `@request_rules_agent`: one `(?i)` RE2 alternation of the keywords on
    `User-Agent`, for agents that contain neither `google` nor a ChatGPT
    agent name (`WELCOME` in `request_rules.py`). Caddy answers 403 itself.
  - `@request_rules_crawler`: the same keywords where the agent does contain
    `google` but no ChatGPT agent name. These go through `forward_auth` to the app's
    `/_host/request-policy` (loopback only, never public: `/_host/*` answers
    404 on every site), which runs the forward-confirmed Googlebot check and
    answers 204 (Caddy continues to the normal route) or 403.

  Requests that match no rule never reach Python, so static files cost one
  regex and one address-set lookup. Only an app that read the table writes the
  file, so a core whose database is unreachable keeps its last include. The
  app runs as `forkmesh-app` and Caddy imports the include as configuration,
  so the app writes only data. The root renderer builds the configuration,
  and it accepts only printable-ASCII keywords of up to 256 characters,
  escaping RE2 metacharacters, backticks, `{` and `$`, so a keyword cannot
  inject Caddy directives or `{$ENV}` substitutions. It also accepts only
  addresses that parse. A non-ASCII keyword is skipped in Caddy and enforced
  only by the app (Python compares latin-1-decoded headers, RE2 compares
  UTF-8). A bad or unreadable export leaves the include as it was. The
  include is runtime state like `tls-*.caddy`, so it is in `IDS_EXCLUDE`.
- **Repository fast path.** `repo_fastpath.py` answers public repository
  reads (`/api/repo/<owner>/<repo>/raw`, tree, blob, ...) from the node's own
  Git gateway before the App sees them; any answer other than 200 falls
  through to the App. Because the App is skipped, the fast path adds the
  App's CORS answer itself: an `Origin` listed in `CORS_ALLOWED_ORIGINS` is
  echoed in `access-control-allow-origin` (with `etag`, `x-forkmesh-ref` and
  `x-forkmesh-served-by` exposed), always with `vary: origin`.
- **Egress.** All subrequests pass `egress.py`: private, loopback, metadata,
  CGNAT and WireGuard addresses are refused at connect time, and every
  redirect hop is rechecked. Without `FORKMESH_SERVES_PRODUCTION=1`,
  `*.forkmesh.com` other than `next.forkmesh.com` is also refused by name;
  the served fleet sets it (`deploy/fleet.toml` `[production]`), so the name
  rule is off there and the address rules stay. The pool speaks HTTP/1.1,
  and offers HTTP/2 only to the APNs hosts (`HTTP2_ONLY_HOSTS`, mounted by
  `http2_mounts`). Every address a name resolves to is checked, and when TCP
  or TLS fails on one the connection moves to the next; when the answer runs
  out it resolves again (re-checked), skipping addresses that failed, for at
  most 4 connections and 8 lookups (`FailoverStream`).
- **Background refresh.** `background.py` puts `env.HOST_BACKGROUND` in the
  app's env: calling it with a key and a coroutine factory runs one task per
  key (at most 16 at once, failures logged), and `.join(key, factory)` makes
  concurrent callers share one load. The app uses it to serve stale copies
  while rebuilding them and to collapse concurrent cache misses; it does not
  exist on Cloudflare, where the same app code runs inline. After the vault
  unseals, `warm_caches` calls `entry.host_warm_caches` once
  (`FORKMESH_HOST_WARM=0` turns it off). See
  [../app/world-boot-latency.md](../app/world-boot-latency.md).

## Protocols and interfaces

- HTTP on `127.0.0.1:8080` behind Caddy; `/readyz`, `/health`,
  `/api/version` (reports `BUILD_REV`). The `/api/metrics/summary` route
  named in PLAN.md's performance gates is not built.
- Mesh listener (`FORKMESH_MESH_LISTEN`, the WireGuard address), requests
  authenticated with HMAC-SHA256 under `FORKMESH_MESH_SECRET`.
- Unseal socket, default `/run/forkmesh-next/unseal.sock`, mode 0600:
  commands `unseal`, `seal`, `seal-restart`, `reload`, `status`, `ops`.
- `web-requests` journal lines (`request_tally.py`, fed by `HostASGI.__call__`
  for every HTTP request under `/api/`):
  `web-requests {"t":<window start ms>,"s":10,"r":[[group,class,count,sum_ms,max_ms],...]}`.
  `group` is `api_metrics._masked_group(path)`, the same identifier-free label
  the desktop's `webRequestRouteGroup` computes, so no user, repository or
  token in a path reaches the journal. `class` is `2xx`..`5xx`; a request whose
  handler raised before answering counts as `5xx`. Durations run from the
  first byte in to the last byte out. A line is written when a request lands in
  a new 10-second window or when the background flusher (started in lifespan
  startup, flushed again on shutdown) passes the boundary, and only when the
  window saw a request. At most 200 route labels, then `other`, as the Worker's
  grouper did. WebSocket upgrades are not counted.

## Configuration

Names only; values come from `deploy/core/templates/app.env` and systemd
credentials rendered by `deploy/core/render.py`.

- `FORKMESH_PLATFORM` (default `vps`), `FORKMESH_ENV_FILE`,
  `FORKMESH_VAR_<NAME>`, `FORKMESH_HOST_DB` (`sqlite:PATH`, `rqlite:URL` or
  `module:callable`), `FORKMESH_HOST_ASSETS_DIR`, `FORKMESH_VAULT`.
- Cluster: `FORKMESH_NODE_ID`, `FORKMESH_MESH_LISTEN`, `FORKMESH_MESH_URL`,
  `FORKMESH_MESH_SECRET`, `FORKMESH_MESH_PEERS`, `FORKMESH_CLUSTER_RQLITE_URL`,
  `FORKMESH_RQLITE_NODE_MAP`; test-only `FORKMESH_HOST_CRON_MINUTE_MS`,
  `FORKMESH_HOST_HEARTBEAT_MS`, `FORKMESH_HOST_STALE_MS`.
- Rate limit: `FORKMESH_RATE_LIMIT_REQUESTS` (default 240, `0` turns it
  off), `FORKMESH_RATE_LIMIT_PERIOD_SECONDS` (10),
  `FORKMESH_RATE_LIMIT_TIMEOUT_SECONDS` (30).
- `FORKMESH_REQUEST_RULES_FILE`: where each rules load writes the export
  for Caddy (`app.env` sets it on the cores; unset means no export).
- `CORS_ALLOWED_ORIGINS` (`FORKMESH_VAR_CORS_ALLOWED_ORIGINS` in `app.env`,
  from `deploy/fleet.toml` `[production.vars]`) is read by the App and by the
  repository fast path.
- `AI_API_URL` enables the AI binding. Production leaves it unset (no AI
  provider). Without it, AI features skip without writing to `error_log`,
  so /status does not count them as errors, and `POST
  /api/ai/workspace-name` answers `ok` with a name built from the prompt's
  words (`"fallback": "missing_binding"`). Package Workers (calorie counter, voice
  changer) answer 503 `missing_binding`/`ai_unavailable`, marked expected
  degraded, so those are not logged either.

## Build, run, test

```sh
python3.13 -m venv .venv && .venv/bin/pip install -r app/host/requirements.txt
cd app/host && ../../.venv/bin/python -m forkmesh_host build
cd app/host && ../../.venv/bin/python -m forkmesh_host serve --port 8080 --db sqlite:/tmp/fm.sqlite --migrate
.venv/bin/python -m pytest app/tests/host
.venv/bin/python -m pytest app/host/tests_rqlite app/host/tests_cluster app/host/tests_unit
```

`tests_rqlite` and `tests_cluster` start real rqlite processes on 127.0.0.1
and skip when `rqlited` is missing (`RQLITED=/path` picks a binary).

## Operations

- Deployed by `tools/next_deploy.py` as part of a release
  ([docs/deploy/README.md](../deploy/README.md)).
- On a node: `journalctl -u forkmesh-app`; `fmnext-unseal --status`;
  `fmnext-app-ops` for one-off account actions, and
  `fmnext-app-ops early-ios-testers all` for the emails of the accounts
  flagged as early iOS testers ([early-ios-testers.md](../app/early-ios-testers.md)).
- Request rules in Caddy, on a node:
  `journalctl -u fmnext-request-rules` shows each render ("N keywords, N
  networks, N skipped, updated|unchanged"), and
  `cat /etc/forkmesh-next/caddy/request-rules.caddy` shows what Caddy
  enforces. After a rule change in the table, every core picks it up on its
  next rules load, at most 60 s later while the app has traffic. The path
  unit then renders and reloads Caddy within about a second. To force a
  render: `systemctl start fmnext-request-rules.service`.
- `python -m forkmesh_host rss` imports everything and prints VmRSS, for the
  1 GB memory budget ([PHASE0_MEASUREMENTS.md](../PHASE0_MEASUREMENTS.md)).

## Security

- `DATA_KEY` exists only in the unsealed process's memory ([VAULT.md](../VAULT.md)).
- The mesh accepts only members from the static map, signed with the mesh
  secret; body caps apply before authentication.
- Egress cannot reach private addresses. Outside the served fleet it cannot
  reach `*.forkmesh.com` either (see Architecture).
- Blocked visitors and aggressive clients are refused before the App runs
  (Architecture, "Blocked requests and rate limit").
- A quarantined node (by peer vote, [IDS.md](../IDS.md)) is excluded from
  Durable Objects, cron and DNS.

## Traps

- Caddy serves static files from `app/dist` and `world/dist` itself. Since
  2026-09-30 the block rules reach those files through the Caddy include
  (Architecture), but the rate limit still covers only what the App answers.
  Caddy has no rate-limit module in the pinned build.
- Caddy's `handle` blocks with a single path matcher sort ahead of named
  ones, so `handle /git/*` (private Git, basic auth) and the per-node
  `handle /readyz` run before the include's 403 handles. `/readyz` is exempt
  anyway. A Google-named agent on `/git/*` still goes through
  `forward_auth`, which Caddy orders before every `handle`.
- In Caddy, `remote_ip` is the TCP peer. For requests that the forkmesh.com
  Worker relays, that peer is a Cloudflare address, so the include's address
  rules miss those visitors. The app still catches them on the routes it
  answers, through the signed visitor header. `/_host/request-policy` checks
  that header against `X-Forwarded-Method`/`X-Forwarded-Uri` without using it
  up (`EdgeClientVerifier.verify(consume=False)`), so the app can still
  accept it once on the real request.
- forkmesh.com is not served by the cores. It is the `forkmesh-www`
  Cloudflare Worker with static assets, so its static pages are covered only
  by the WAF rule that `tools/forkmesh-sync-block-rules.sh` publishes from the
  same table. Rules added since the last sync are missing there until that
  script runs again. Paths the Worker relays to app.forkmesh.com get the
  cores' rules as well.
- A Caddy path matcher ignores case, while the app's exempt check does not:
  `/HEALTH` is exempt in Caddy but not in the app.
- An app release older than 2026-09-30 has no `/_host/request-policy`. After
  a rollback to one, `forward_auth` passes on that release's 404, so
  Google-named agents that match a keyword get 404 from Caddy. Address
  matches and other keyword matches still get 403. Undo it with
  `printf '# off\n' >/etc/forkmesh-next/caddy/request-rules.caddy && systemctl
  reload caddy`; the old release writes no export, so the include stays that
  way until a newer release loads the rules again.
- The edge-client MAC is single use, so `HostASGI._verified_edge` verifies it
  once per request and keeps the result in the ASGI scope; the policy check
  and `_request` both read that result. Verifying again would count as a
  replay and fall back to the Worker's Cloudflare address.
- From the Vultr cores, Akamai (Apple's DNS) answers `api.push.apple.com`
  with `54.65.172.3` about half the time, over UDP and TCP and straight from
  its authoritative servers. That is an EC2 host serving `*.value-domain.com`,
  so TLS fails with "Hostname mismatch", and glibc then returns that one
  address with no IPv6 alternative. 1.1.1.1 and 8.8.8.8 never gave it. Before
  the failover above, about half of all pushes were lost this way even with
  HTTP/2 (measured 2026-09-28: 7 of 20 reached Apple; 60 of 60 with it).
- APNs accepts HTTP/2 only. Over HTTP/1.1, `api.push.apple.com` presents a
  certificate for another name, so every push fails at connect, and
  `apns.send_one` reports that as status 0 without a log line. The egress pool
  needs `h2` installed and the APNs hosts in `HTTP2_ONLY_HOSTS`.
- Do not offer HTTP/2 to other origins. httpcore (1.0.x) opens each HTTP/2
  stream with a 16 MiB receive window, the same size as the whole
  connection's window. It never returns credit for bytes that arrived for a
  stream the caller closed or dropped before reading its body. Code that checks only a
  status (the `/status` release-blob probes, a cancelled download through
  the mirror proxy) therefore uses up the connection's window. After that,
  every later request to that origin on the pooled connection gets its
  headers and then waits for a body that never arrives, until the connection
  closes. Over HTTP/1.1, closing an unread body closes just that connection.
  `test_abandoned_body_does_not_stall_the_next_read_from_that_origin` runs
  this against a local HTTP/2 server.
- The app runs on Debian's Python 3.13 in the fleet, while the Worker code
  targets Pyodide's 3.14 (`uv` is not used on nodes).
- `serve` rebuilds `app/src_build` when any `app/src` or `world/src` module
  is newer than the staged `entry.py`; `--no-build` skips that.
- `app/tests/host/conftest.py` puts `app/src_build` on `sys.path` at import
  time. In a fresh checkout that directory does not exist yet, so Python
  caches "no finder" for it; the `staged_src` fixture builds it and must call
  `importlib.invalidate_caches()`, or `import schema` in the same pytest
  process fails with `ModuleNotFoundError` (seen 2026-09-28 running
  `test_next_rekey.py` in a new worktree). The deploy gate prestages
  `src_build` before pytest, so it never hit this.
- `test_next_rekey.py::test_every_blind_index_column_is_handled` and
  `test_every_ciphertext_token_and_dropped_table_exists` fail whenever a
  migration adds a `*_bi` column or a table without registering it in
  `tools/next_rekey.py` and [REKEY.md](../REKEY.md). That blocked auto-deploys
  on 2026-09-27 (code challenges, fixed by 4ee96968d1) and 2026-09-28
  (`branch_op_results`, fixed by 9fc5eacf17). Run the file with any migration.
- `app/host/forkmesh_host/repo_ssr.py` cites `docs/PERF_REPO_PAGE.md`; the
  doc is [PERF_PAGES.md](../PERF_PAGES.md).
- uvicorn's `websockets-sansio` protocol logs "returned without completing
  handshake" after every `websocket.http.response` denial, even though the
  denial is valid ASGI. Refuse WebSockets through `HostASGI._deny` so the
  filter applies. Replacing the HTTP denial with `websocket.close` would make
  uvicorn answer 403, and clients would read that as an auth failure instead
  of retrying.
- rqlite's `/status` gathers a `network` reporter over netlink, which the
  rqlite unit's `RestrictAddressFamilies` forbids, so every `/status` call
  logs "failed to retrieve stats for registered reporter network". The
  membership view asks every second, so it asks `/leader` (the leader's `id`,
  503 without a leader) and falls back to `/status` only on an rqlite that
  answers 404 there. On 2026-09-24 the `/status` poll wrote about 100k journal
  lines a day per core and pushed everything else out of the 64 MB journal
  within 6 to 8 hours.
- A restarted app is sealed for about a second, until a peer unseals it
  (`vault: sealed … asking peers` → `vault: unsealed`). Caddy retries only
  connection failures (`lb_try_duration`), not a `503 {"error": "sealed"}`, so
  every roll answered 503 to whatever arrived in that second:
  `/api/nodes/events`, room and status WebSockets, the daemon's replica and
  fork relays. Within 20 s of process start, before the first unseal, a
  request or WebSocket handshake now waits up to 3 s for the unseal, then
  answers 503 as before. `/health` and `/readyz` never wait. Neither does a
  node that was unsealed once and then sealed by the operator, nor a
  quarantined node.

## Recent changes

- 2026-10-13: the App adds `request_block_rules` rows itself: a visitor whose
  404s are scanner probes gets an `auto-probe-*` `ip` (or IPv6 `/64` `cidr`)
  row with priority 0, deleted when the ban ends. Nothing here changed; the
  rows load and render into Caddy like any other
  ([../app/probe-auto-ban.md](../app/probe-auto-ban.md)).
- 2026-10-07: ChatGPT is no longer answered `403 Forbidden`. The generic
  `bot` keyword rule matched `ChatGPT-User/1.0; +https://openai.com/bot`
  and `OAI-SearchBot`, so forkmesh.com (Cloudflare WAF), app.forkmesh.com
  and world.forkmesh.com refused every page ChatGPT fetched for a user or
  for its search index. User-agent keyword rules now skip any User-Agent
  containing `chatgpt-user` or `oai-searchbot`, on every path, in the app
  (`request_policy.WELCOME_AGENTS`), the cores' Caddy include
  (`request_rules.WELCOME`) and the forkmesh.com WAF expression
  (`sync_request_block_rules.WELCOME_AGENTS`). The exemption is by name only,
  unlike Google's: a keyword rule only stops clients that announce
  themselves, and anything that fakes these names could equally send a
  browser User-Agent. Address rules and the rate limits still apply.
  `GPTBot` (OpenAI's training crawler) is not exempt. The WAF picks this up
  only when `tools/forkmesh-sync-block-rules.sh` is run. Tests:
  `app/tests/host/test_host_request_policy.py`,
  `deploy/tests/test_request_rules.py` (live Caddy),
  `app/tests/test_sync_request_block_rules.py`.
- 2026-10-02: the egress guard reuses a name's last answer when a lookup
  fails. The retry below was not enough. On the cores the Vultr resolvers
  (`108.61.10.10`, `108.61.115.10`; no local cache, systemd-resolved is
  off) failed for 7 or more minutes in a row, and from 03:00 to 08:00 UTC the
  `/status` "World mobile startup" row failed 68 of 300 probes. The
  failures were either `[Errno -3]` after every retry, or a lookup that hung
  for the whole 10 s connect timeout. A hung lookup raises
  `asyncio.TimeoutError`, whose text is empty, so the row and the Ping read
  just `cannot resolve world.forkmesh.com:`. World was fine on phones,
  which use their own resolvers. `_lookup` in `egress.py` now keeps the
  last successful answer for each name (`_last_answers`, up to
  `RESOLVE_STALE_NAMES` names). It serves that answer, as RFC 8767
  "serve-stale" allows, when a lookup fails with `EAI_AGAIN`, `EAI_NODATA`
  or `EAI_FAIL` after its retries, or times out. This is allowed only for
  answers less than `RESOLVE_STALE_MAX_AGE` (1 h) old. While such an answer
  exists, the lookup waits at most `RESOLVE_STALE_AFTER` (2 s), so the
  connect still has time left. The reused addresses go through the same
  public-address check as a fresh answer, so a name that was allowlisted
  then is refused if it no longer is. `EAI_NONAME` (the name is gone) never
  reuses an answer. `stats["stale_answers"]` counts the reuses. A timeout
  now reads `cannot resolve <name>: lookup timed out`.
- 2026-10-02: the egress guard retries a transient lookup failure.
  `resolve_all_checked` made one `getaddrinfo` call, and the cores' Vultr
  resolvers answer about 1 in 100 lookups with a fast (<0.1 s)
  `EAI_AGAIN` ("Temporary failure in name resolution") or `EAI_NODATA`.
  Measured on n2 and n3 that day, a lookup 0.25 to 0.5 s later succeeded
  every time. In bursts the rate reached 13 of 60. So the `/status` "World
  mobile startup" row (the only synthetic probe that fetches over the
  network; the homepage and status-page probes read `ASSETS`) failed for
  runs of several minutes with
  `cannot resolve world.forkmesh.com: [Errno -3]`, Pinged "World mobile
  startup needs attention", and the mirror daemon's endpoint registration
  failed the same way. `_getaddrinfo` now retries `EAI_AGAIN`,
  `EAI_NODATA` and `EAI_FAIL` after 0.25 s and 0.5 s
  (`RESOLVE_RETRY_DELAYS`), within the lookup's own timeout. `EAI_NONAME`
  (the name does not exist) fails at once, as before.
- 2026-10-01: a held runner claim no longer delays an app restart. At the
  00:48 deploy of `16621cf4f961`, n3's app took 10.8 s to stop (04:48:47.2
  to 04:48:58.0 UTC), then 4.5 s to start again. uvicorn's graceful shutdown
  was waiting for r1's `POST /api/runners/claim`, which polls the database
  once a second for up to 20 s. Port 8080 was closed for about 16 s, longer
  than Caddy's 10 s `lb_try_duration`. Every request that arrived early in
  that window got `502 dial tcp 127.0.0.1:8080: connect: connection refused`
  after exactly 10 s: the desktop's chat `room-access` calls for each channel
  and DM, `/api/nodes/events` and the room WebSockets. The runtime now puts
  an `asyncio.Event` at `env.HOST_STOPPING`. `serve.py` hands uvicorn an HTTP
  protocol (`ws_protocol.stopping_protocol`) whose `shutdown()`, called on
  every open connection when shutdown begins, sets the event.
  `runner_leases.WorkerRuntime.sleep` wakes on it, and the claim's wait
  answers with no jobs, so the runner claims again at once. The
  event is only a signal: nothing is cancelled, and a claim that is leasing
  jobs finishes as before. Tests:
  `app/host/tests_unit/test_ws_shutdown_answer.py`
  (`test_shutdown_wakes_a_held_request_so_the_app_exits_promptly`),
  `app/tests/test_runner_leases.py`.
- 2026-09-30: `request_block_rules` reach everything the cores serve, not
  only what the app answers. Measured that day, a Bytespider agent got 200
  from world.forkmesh.com, and an agent matching an existing keyword rule got
  200 from forkmesh.com's homepage and the app's 308 redirect. Caddy now
  imports a rules include rendered from the app's export (Architecture,
  "Request rules in Caddy"). User-agent keyword rules apply to
  `/api/chat/agent-guestbook` and `/api/agent-notes` again, in the app and in
  the forkmesh.com WAF expression (`app/tools/sync_request_block_rules.py`).
  This was the owner's choice: ChatGPT's guestbook GETs answer 403 again
  under the generic keyword that matches `+https://openai.com/bot`
  ([../app/agent-notes.md](../app/agent-notes.md)). Tests:
  `app/tests/host/test_host_request_policy.py`,
  `deploy/tests/test_request_rules.py` (live Caddy).
- 2026-09-30: a WebSocket handshake still pending when the app stops now gets
  `503 Service Unavailable` with `Retry-After: 5` and
  `X-Forkmesh-Expected-Degraded: 1`. uvicorn's `websockets-sansio` protocol
  answered it with a bare `500 Internal Server Error` (21 bytes, nothing in
  the app's log), while in-flight HTTP requests are left to finish. At the
  23:17:04 deploy restart of n3, the iOS app's relay socket
  `/api/repo/mainnode/forkmesh/rooms/world-general/ws` was mid-handshake (the
  app was still looking up the room's repository), and the desktop showed
  "Node n3 core caddy err: … -> 500". `serve.py` passes
  `ws_protocol.DrainingWebSocketProtocol` to uvicorn. Sockets that are
  already open still close with 1012. The marker keeps the 503 out of the
  desktop's ERROR category, and the phone reconnects as it does for any
  refused handshake. Caddy does not replay it: `lb_try_duration` retries
  only failed connections, and restarts sometimes outlast its 10 s window
  (502s on n1 at 22:24 and on n2 at 23:13). The internal mesh listener keeps
  the stock protocol. Test: `app/host/tests_unit/test_ws_shutdown_answer.py`.
- 2026-09-30: `d1_rqlite.py` maps rqlite's per-statement Raft errors
  (`timed out enqueuing operation`, `raft is already shutdown`, alongside the
  existing leader-loss and deadline messages) to `D1 DB is overloaded`, so the
  app answers 503 with backpressure. rqlite reports these inside an HTTP 200
  body, which skipped the transport-level overload check; a leader whose Raft
  apply queue was full turned
  `/api/orgs/forkmesh/discord/messages` into a 500 and a System alert
  reading `D1Error('D1_ERROR: timed out enqueuing operation: SQLITE_ERROR')`.
  The `D1` names are the adapter's compatibility surface for the app; the
  database is rqlite, not Cloudflare D1. Test:
  `app/host/tests_unit/test_d1_raft_errors.py`.
- 2026-10-06: `d1_rqlite.py` also maps `index N: timeout waiting for fsm`
  to `D1 DB is overloaded`. Raft committed the entry but the leader's state
  machine did not apply it in time, so the statement's outcome is unknown and
  the app answers 503 instead of a 500. It had surfaced as
  `D1Error('D1_ERROR: index 3589896: timeout waiting for fsm: SQLITE_ERROR')`
  from `org_agent_bot_handler`. Test:
  `app/host/tests_unit/test_d1_raft_errors.py`.
- 2026-09-30: `request_policy.py` no longer applies user-agent keyword rules
  to `/api/chat/agent-guestbook` and `/api/agent-notes`; a keyword rule had
  been answering ChatGPT's guestbook GETs 403 since 2026-09-28
  ([../app/agent-notes.md](../app/agent-notes.md)).
- 2026-09-30: The repository fast path (`repo_fastpath.py`) serves
  `merge-preview` like `compare`: the same `base`/`head` query, `no-cache`
  with an ETag, and `immutable` (plus host memory) when both are full commit
  ids, since a merge trial between two fixed commits cannot change. The
  gateway has served it on its public route since f22c0eb16, so it had stayed
  on the app only by omission. `READ_OPERATIONS` is the gateway's public
  reads that the app routes as plain reads; `release-blob` stays on the app
  because it needs the release digest from the database (this used to be a
  source comment). Any non-200 answer still falls through to the
  app, which now routes it too ([app/README.md](../app/README.md), "Recent
  changes").
- 2026-09-28: the cores enforce `request_block_rules` and a per-visitor rate
  limit (`request_policy.py`, Architecture). Until now the table was enforced
  only by Cloudflare's WAF for forkmesh.com; app.forkmesh.com and
  world.forkmesh.com, which Cloudflare does not proxy, answered blocked
  crawlers with 200 and had no rate limit.

- 2026-09-28: outbound HTTP/2 is limited to the APNs hosts. Since
  2026-09-27 every origin was offered HTTP/2, and the cores' reads of each
  other's gateways stalled once a dropped release-blob body had used up a
  connection's flow-control window. That made the `/status` homepage
  downloads row go red about every third minute (see Traps and
  [../app/README.md](../app/README.md)).

- 2026-09-28: pushes no longer fail when Apple's DNS hands the cores a
  non-Apple address: the guarded egress backend fails over to the name's
  other checked addresses and looks the name up again (see Traps).
- 2026-09-28: `app/tests/host/test_repo_fastpath.py` builds its `RepoFastPath`
  with a 10 s connect timeout (`_fast_path`). The production client keeps its
  0.5 s connect, which is meant to fall through to the app when the local
  gateway is slow. In the auto-deploy gate, the tests run while the release
  builds, so the in-process stub gateway sometimes missed 0.5 s. The read then
  fell through and `test_unchanged_read_answers_304` failed with nothing to
  unpack, blocking a deploy of 25158b5f142d with no code change behind it.
- 2026-09-28: `test_invalid_edge_client_keeps_caddys_address` builds each
  edge-client header when the test runs. The values used to be built at
  collection time, so in a run that reached this test more than 100 s after
  collection, the "400 s in the future" header was only about 290 s ahead,
  inside the 300 s skew window. It was accepted and the test failed. This file
  is in the auto-deploy gate's must-pass set.
- 2026-09-27: phone pushes work again. Since the cutover to this host no APNs
  push had been delivered: `fetch` spoke HTTP/1.1, which Apple refuses. The
  guarded egress pool now offers HTTP/2 (`h2` 4.4.1, `hpack`, `hyperframe`).

- 2026-09-27: no more WebSocket 503s while a core restarts. The 503s seen on
  n2 on 2026-09-25 (21:31:55 to 21:32:04) came from stale rows, not from
  unsealing. A stopped core's row stayed `active` until its heartbeat was
  15 s old. Meanwhile peers forwarded its rooms to a mesh port nobody
  answered, and the app answered `503 {"error":"unavailable"}` after one
  retry. `next_deploy` restarts n2 and n3 together, so the first core back
  hit this for the other one's rooms. The fixes:
  - a stopping core drains (see Architecture);
  - the forwarder retries once on the owner's own `503 no quorum` (marked
    `x-forkmesh-mesh: no-quorum`, so an object's own 503 is never
    replayed), or on a refused connection whose owner has moved;
  - `next_deploy restart` waits 3 s after each node's gate before the next
    node drains, so every core has it back as live.

  `test_graceful_owner_restart_hands_rooms_over_without_503`
  (`tests_cluster`) SIGTERMs the world's owner and joins the world on the
  first try through every core, before and after the restart. A crash
  (SIGKILL) still waits out the 15 s stale window.

  The harness now also waits until every core's view lists all three before
  the first test. A core is ready once it sees itself, which can be about a
  second before it sees its peers. On main too, the first durable test then
  sometimes found three owners for the world, and the rest of the module
  failed after it. `test_replica_mode`'s fake rqlite gained `leader()`, which
  the view has called since the `/leader` change.

- 2026-09-25: the homepage's download list (`www/public/homepage-downloads.js`,
  which fetches the signed release manifest from app.forkmesh.com) failed in
  every browser with "Release metadata unavailable": fast-path repository
  reads carried no CORS headers. The fast path now adds them (b6b2dc4cc).
- 2026-09-25: no more `ERROR: ASGI callable returned without completing
  handshake.` when a WebSocket is refused with an HTTP status (sealed vault,
  or a Worker 503 while a Durable Object's node restarts, as at the
  2026-09-24 19:35 restart). `_deny` did send a complete
  `websocket.http.response`, and the client got the 503. The ERROR came from
  uvicorn 0.53/0.54's `websockets-sansio` protocol, which never marks that
  path as a completed handshake. `asgi.install_handshake_log_filter` drops
  that one message on `uvicorn.error`, and only for a connection whose
  `_deny` sent the HTTP answer (a context variable set in the connection's
  task). A handler that really returns without answering still logs it.
- 2026-09-24: log-error pass (`fix/log-errors`): membership polls rqlite
  `/leader` instead of `/status`; denied WebSocket handshakes no longer log
  an ERROR line; requests in the boot unseal second wait for it instead of
  answering 503 (see Traps).

- 2026-09-24: `web-requests` journal lines so the desktop's Web Requests tab
  counts the cores' traffic. Aggregated per 10 seconds instead of one access
  line per request: the journal is capped at 64 MB, and at a few requests a
  second per-request lines would push the IDS, rqlite and app history out of it
  within hours; a batch line is at most one every 10 seconds per node.

- 2026-09-24: leaderless cron, membership and DNS votes (ac7c6d3); Host
  allowlist, body caps, egress guard, edge-client HMAC (02f5237); repository
  fast path and SSR first views (c2d47d4, 74cc366, 8b639c0, 3be0bd7); IDS
  quarantine (6df712f); confirm local-read misses with the leader (0adec3d).
- 2026-09-23: sealed vault (b8b95bf); Durable Objects across three nodes
  (cae6f88); rqlite D1 adapter (a47559a); CPython host shim (ba780c5).

## Related docs

[PLAN.md](../PLAN.md) · [OPERATIONS.md](../OPERATIONS.md) ·
[VAULT.md](../VAULT.md) · [IDS.md](../IDS.md) · [PERF_PAGES.md](../PERF_PAGES.md) ·
[docs/deploy/README.md](../deploy/README.md) · `app/host/RQLITE_FINDINGS.md`
