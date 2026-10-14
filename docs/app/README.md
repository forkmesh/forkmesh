# App (`app/`)

The App is the ForkMesh control plane: the web dashboard, every `/api/*`
route, account sessions, Git Smart HTTP for hosted and proxied repositories,
ActivityPub, the chat and World Durable Objects, the per-minute cron, and the
database. It serves `app.forkmesh.com` and, with `world/`,
`world.forkmesh.com`. The Python code (`app/src`, staged to `app/src_build`)
runs on CPython on the core nodes under the host shim in
`app/host/forkmesh_host/`: rqlite stands in for D1, Durable Objects run
clustered across the cores, and Caddy serves static files. Agent jobs run on
the runner tier. The host is documented in
[docs/host/README.md](../host/README.md); this page covers the App code.

The code keeps a Cloudflare Worker shape: a Worker entrypoint, bindings named
after D1, KV, Queues and Durable Objects, and `app/wrangler.toml`. The
forkmesh.com site proxies its dynamic traffic here (see
[docs/www](../www/README.md) and [docs/world](../world/README.md)). The
desktop, the Go daemon, the runner, the mobile apps and browsers are the
App's clients. Public, user-facing descriptions live in `www/docs/`.

## Code map

| Path | Responsibility |
| --- | --- |
| `app/src/app_runtime.py` | The `Default` WorkerEntrypoint: `fetch`, `scheduled`, `queue`; stateless fast paths and the handoff to `entry.py` |
| `app/src/entry.py` | The router, most route handlers, the Durable Object classes (`ForkMeshRoom`, `ForkMeshNodes`, `ForkMeshNoteRoom`, `ForkMeshCronRunner`, `ForkMeshCronWatchdog`, `ForkMeshDiscordGate`), crypto helpers, `ensure_schema` |
| `app/src/*.py` (other modules) | Domain modules loaded on demand through `_LazyModule` |
| `app/src/schema.py` | Runtime DDL and the schema fingerprint stored in `schema_meta` |
| `world/src/*.py` | World, Office and organization task routes and the `ForkMeshWorld` / `ForkMeshOfficeRoom` Durable Objects; staged into the App |
| `marketplace/packages/*/content/worker/*.py` | Server code of marketplace packages, staged next to the App modules |
| `app/migrations/NNNN_*.sql`, `app/migrations/down/` | Numbered roll-forward migrations and their down scripts (World migrations too) |
| `app/wrangler.toml` | Not deployed. Source of `[vars]`, binding lists, queue consumer settings and `run_worker_first`, read by `app/host/forkmesh_host/env.py` and `deploy/core/caddy_routes.py` |
| `app/host/` | The CPython host: ASGI front, bindings, rqlite D1 adapter, clustered Durable Objects, vault, request guard, IDS records |
| `app/public/` | Static pages (login, signup, dashboard shells, chat, notes, status) |
| `app/public/dashboard/js/*.js` | Dashboard fragments, concatenated into `app/public/dashboard.js` by `app/tools/build_dashboard_assets.py` |
| `app/tools/build_worker_python.py` | Stages `app/src`, `world/src` and package modules into `app/src_build/` with comments stripped |
| `world/tools/build_worker_footprint.py` | Source footprint report and static budgets |
| `app/tests/`, `app/tests/host/`, `app/browser_tests/` | Python tests, host tests, Playwright specs |

## Topic pages

| Area | Pages |
| --- | --- |
| Platform | [cron-and-durable-objects](cron-and-durable-objects.md), [d1](d1.md), [worker-footprint](worker-footprint.md), [auth-sessions](auth-sessions.md), [login-page](login-page.md), [update-notice](update-notice.md), [docs-book](docs-book.md), [world-boot-latency](world-boot-latency.md), [dashboard-colors](dashboard-colors.md), [dashboard-components](dashboard-components.md) |
| Repositories | [repo-views](repo-views.md), [repository-file-edits](repository-file-edits.md), [branch-ops](branch-ops.md), [remote-forks](remote-forks.md), [provider-mirrors](provider-mirrors.md), [public-pages](public-pages.md) |
| Chat | [chat-private-rooms](chat-private-rooms.md), [chat-direct-calls](chat-direct-calls.md), [chat-push-previews](chat-push-previews.md), [chat-read-sync](chat-read-sync.md), [chat-unread-reminders](chat-unread-reminders.md), [chat-everyone-channels](chat-everyone-channels.md), [chat-formatting](chat-formatting.md), [web-chat-layout](web-chat-layout.md), [web-composer](web-composer.md) |
| Notifications | [notification-email](notification-email.md) |
| Tasks | [task-progress](task-progress.md), [task-markdown](task-markdown.md), [task-trash](task-trash.md), [task-pings](task-pings.md), [task-email](task-email.md), [task-board-filters](task-board-filters.md) |
| Agents and desktop | [agents-page](agents-page.md), [desktop-agents-web](desktop-agents-web.md), [desktop-commands](desktop-commands.md), [settings-agents-and-runners](settings-agents-and-runners.md), [agent-notes](agent-notes.md), [activity-today](activity-today.md), [workflows](workflows.md) |
| Accounts and organizations | [member-roster](member-roster.md), [organization-bot-scopes](organization-bot-scopes.md), [payments-and-plans](payments-and-plans.md), [badges](badges.md), [challenges](challenges.md), [feedback-bounties-links](feedback-bounties-links.md), [early-ios-testers](early-ios-testers.md) |
| Operations and growth | [analytics](analytics.md), [not-found-tracking](not-found-tracking.md), [probe-auto-ban](probe-auto-ban.md), [signup-failure-pings](signup-failure-pings.md), [marketing-email](marketing-email.md), [ecosystem](ecosystem.md), [package-ai-failures](package-ai-failures.md) |

## Architecture

**Runtime.** `forkmesh_host.runtime` loads the Worker entry module unchanged
and drives `fetch`, `scheduled` and `queue`. `forkmesh_host.asgi` turns HTTP
and WebSocket requests into `Default.fetch` calls under uvicorn
(`python -m forkmesh_host serve`). Static files are served by Caddy; paths in
`run_worker_first` go to the App.

**Bindings** (`app/host/forkmesh_host/env.py`). `DB` is rqlite through
`d1_rqlite.py`; `REPOSITORY_METADATA` is a local SQLite KV per node;
`NOTIFICATION_QUEUE` is a local queue; `ASSETS` serves `app/dist`; `AI`
exists only when `AI_API_URL` names an OpenAI-compatible endpoint; Durable
Object namespaces run across the cluster (`durable_cluster.py`).

**Request path.** `app_runtime.Default.fetch` answers identity routes
(`/api/version`, `/health`, the root redirect) without the database.
Everything else goes through `_app_ingress_fetch` in `entry.py`. Mirror sync
traffic, Git clone reads and a short list of heavy routes are routed into one
of sixteen `FORKMESH_CRON_RUNNER` ingress objects (`_app_ingress_shard`); if
that object is unavailable the request gets a marked 503
(`EXPECTED_DEGRADED_HEADERS`, `retry-after: 30`) instead of running inline.

**Durable Objects.** `ForkMeshRoom` (chat rooms and relay sockets),
`ForkMeshNodes` (per-owner event sockets), `ForkMeshNoteRoom` (live notes),
`ForkMeshWorld` and `ForkMeshOfficeRoom` (World presence and Office meetings),
`ForkMeshCronRunner` (cron coordinator and ingress shards),
`ForkMeshCronWatchdog`, `ForkMeshDiscordGate`. Each object is placed on one
live node by rendezvous hash; storage and alarms live in rqlite. See
[cron-and-durable-objects.md](cron-and-durable-objects.md).

**Storage.** `DB` is the database of record and keeps D1's SQL dialect. Most
row payloads are AES-GCM blobs under `DATA_KEY` with HMAC blind indexes for
lookups. Git bytes are never stored in the database; they live on desktop
nodes and in the cores' Git stores ([HOSTED_REPOS.md](../HOSTED_REPOS.md)).
See [d1.md](d1.md).

**Cron.** Each minute the host calls `scheduled`, which rings the
`scheduled-runner-v1` coordinator; the coordinator rotates across live nodes
and its alarm runs the phases due that minute under
`CRON_BATCH_SUBREQUEST_CAP`, chaining alarms when the budget runs out
(`host_cron_runs` records which node ran which minute).

**Queue and push.** `NOTIFICATION_QUEUE` carries a payload-free doorbell for
instant notification email, sent through Postfix on the node's loopback
(`MAIL_TRANSPORT = "smtp"`); the cron is the backstop. Clients do not poll for
display data: changes reach them as frames on the node event socket
(`ForkMeshNodes`) and as APNs pushes (`app/src/apns.py`).

**Secrets.** Nodes start sealed; `DATA_KEY` and other vaulted secrets arrive
over the unseal socket ([VAULT.md](../VAULT.md)).

## Protocols and interfaces

- HTTP API under `/api/*`, canonical base `https://app.forkmesh.com/api`.
  The desktop's endpoint catalog is
  `desktop/resources/network/endpoint-catalog.json`.
- The site proxy vouches for the visitor with the signed
  `x-forkmesh-edge-client` header, verified by the host.
- Git Smart HTTP: `app/src/git_http.py`, `app/src/hosted_git.py`.
- ActivityPub and NodeInfo under `/ap/*`, `/.well-known/*`, `/nodeinfo/*`,
  `/@name` (`app/src/activitypub.py`).
- Remote MCP endpoint at `/mcp` (`app/src/remote_mcp.py`).
- Outbound repository webhooks (`forkmesh-webhook-v1`,
  `app/src/repository_webhooks.py`): events `repository.push`,
  `branch.push`, `pull.merged`, `release.published`; queued and drained by the
  `webhook-outbox` cron phase with up to 6 attempts; private repositories emit
  nothing. The versioned integration contract is `www/docs/integrations.md`.
- Public snapshots: anonymous GET routes served from validated
  `public_api_snapshots` rows (`app/src/public_snapshots.py`).
- Organization bot tokens (`fmbot_`):
  [organization-bot-scopes.md](organization-bot-scopes.md).

## Configuration

- Non-secret vars: `[vars]` in `app/wrangler.toml` (origins, `NODE_NAME`,
  `WORKER_ROLE`, `HOSTED_GIT_CORE_NODES`, `MAIL_TRANSPORT`,
  `TURNSTILE_SITE_KEY`, and others). Optional overrides are listed there as
  comments with their defaults.
- Overrides, in increasing precedence: an env file (`--env-file` /
  `FORKMESH_ENV_FILE`), `FORKMESH_VAR_<NAME>` process variables, and systemd
  credentials. `[production.vars]` in `deploy/fleet.toml` is rendered into the
  nodes' `app.env`.
- Secrets: bootstrap credentials per node (`deploy/core/render.py`); the
  rest, `DATA_KEY` and `ADMIN_PASS` included, sealed in `host_secrets`
  (`tools/next_vault.py`).
- Runtime: Debian's Python 3.13 on the nodes.

## Build, run, test

```sh
python3 app/tools/build_dashboard_assets.py      # after editing app/public/dashboard/js/*
python3 app/tools/build_worker_python.py         # stage app/src_build/
python3 world/tools/build_worker_footprint.py --staged   # check budgets
python3 -m pytest app/tests/host                 # host tests
python3 -m pytest -q app/tests/test_<area>.py    # one App module at a time
python3 tools/next_deploy.py build               # plan a release build (dry run)
```

Run the App locally with `python -m forkmesh_host serve` and a SQLite
database. Run `app/tests` in small batches; the whole suite at once can
exhaust memory. Browser tests:
`cd app/browser_tests && npm ci && npx playwright test <spec>`.

## Operations

- Releases, migrations and rolling restarts:
  [docs/deploy/README.md](../deploy/README.md) and
  [OPERATIONS.md](../OPERATIONS.md). A node that fails its gate reverts and
  the rollout stops. Check the rev at `/api/version` before assuming a fix
  is live.
- Logs: `journalctl -u forkmesh-app` on a node, or the desktop Log page.
  Application errors land in `error_log` and Sentry. `log_error` redacts a
  request path that could name a private repository; database-unavailable
  errors keep a fixed kind (`overload`, `too many`, `internal error`,
  `network connection lost`, `other`) through redaction.
- The first row of each new `error_log` group pings administrators;
  database-unavailable rows are recorded but do not ping, since they are
  retryable 503s.
- Cron costs per tick: `GET /api/accounts/admin-cron-report`.

## Security

- `DATA_KEY` encrypts row payloads; blind indexes are HMACs. There is no
  key-version byte, so the key is rotated only offline with the apps stopped
  (`tools/next_rekey.py`, [REKEY.md](../REKEY.md)).
- The host applies a Host allowlist, streamed body caps, an SSRF egress guard
  and edge-client verification before the App sees a request
  (`request_guard.py`, `egress.py`).
- Signup forms require Turnstile (`app/src/turnstile.py`).
- The admin console is behind `ADMIN_PATH` and `ADMIN_PASS` with a 12-hour
  cookie; its Database tab hides and write-blocks host tables such as
  `host_secrets`.
- ActivityPub delivery is off unless `AP_OUTBOUND` is set.
- Webhook destinations must be public routable https URLs outside the
  ForkMesh domains (checked again at send time); URL and secret are stored
  encrypted.
- No card data reaches the App ([payments-and-plans.md](payments-and-plans.md)).

## Traps

- **`app/wrangler.toml` is not deployed.** Only `[vars]`, binding lists,
  queue settings and `run_worker_first` matter.
- **Lazy modules.** A new module imported through `_LazyModule` belongs in
  `LAZY_MODULES` in `world/tools/build_worker_footprint.py`.
- **Served line numbers do not match `app/src`.** The host runs the compacted
  `app/src_build/`; regenerate it to read a traceback.
- **Stale `dashboard.js`.** Edits under `app/public/dashboard/js/` do nothing
  until the bundle is rebuilt.
- **Source-text tests.** Many tests read `entry.py` as text; moving a function
  between modules can break a test without changing behaviour.
- **Schema fingerprint.** Any edit to `app/src/schema.py` makes the next cold
  start replay DDL once (single-flighted in `ensure_schema`).
- **Migrations are roll-forward.** A code rollback must accept the newest
  schema.
- **rqlite freezes `RANDOM()` and `'now'` in triggers and views**;
  `tools/rqlite_migrate.py` sends such files without the rewrite.
- **A local rqlite read can miss a fresh write** on another node; the adapter
  confirms misses with the leader.
- **Organization aliases.** Organization repositories reach handlers with
  `owner` rewritten to the backing node (`org_alias_rewrite`). Anything public
  (ActivityPub handles, note text, links) must use the organization name from
  the original URL; privacy and repository rows use the node.
- **Repository read actions.** Every action in `REPO_HOST_RE`
  (`app/src/urls.py`) must be in `REPO_HOST_READ_ACTIONS`
  (`app/src/app_runtime.py`), or it answers the cacheable 410
  `repository_socket_transport_retired`;
  `app/tests/test_repo_read_route_operations.py` checks the lists agree, and
  `app/tests/test_dashboard_landing_migration.py` reads the set's members
  from source to check the dashboard's compare, history, branch and raw
  reads stay on the direct HTTPS gateway.
- **Read guard.** Heavy readers go through `app/src/d1_read_guard.py`
  `shared_read`; the switch is off by default on rqlite.

## Related docs

- [docs/host/README.md](../host/README.md),
  [docs/deploy/README.md](../deploy/README.md), [PLAN.md](../PLAN.md),
  [OPERATIONS.md](../OPERATIONS.md), [VAULT.md](../VAULT.md)
- `www/docs/operations/polling-elimination.md` (push-first client contract),
  `www/docs/integrations.md`, `www/docs/design/activitypub-federation.md`
