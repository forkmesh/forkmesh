# App (`app/`)

The App is the ForkMesh control plane: the web dashboard, every `/api/*`
route, account sessions, Git Smart HTTP for hosted and proxied repositories,
ActivityPub, the chat and World Durable Objects, the per-minute cron, and the
database. It serves `app.forkmesh.com` and, with `world/`,
`world.forkmesh.com`. The Python code (`app/src`, staged to `app/src_build`)
runs on CPython on the three core nodes `fmnext-n1` to `fmnext-n3`, under the
host shim in `app/host/forkmesh_host/`: rqlite stands in for D1, Durable
Objects run clustered across the cores, and Caddy serves static files. Agent
jobs run on the runner tier (`fmnext-r1`). The host itself is documented in
[docs/host/README.md](../host/README.md); this page covers the App code.

Until 2026-09-24 the same code ran as the Cloudflare Python Worker
`forkmesh-app` with D1. It still carries that design: a Worker entrypoint,
bindings named after D1, KV, Queues and Durable Objects, and
`app/wrangler.toml`. forkmesh.com is the `forkmesh-www` Worker, which proxies
its dynamic traffic here (see [docs/www](../www/README.md) and
[docs/world](../world/README.md)). `app.next.forkmesh.com` and
`world.next.forkmesh.com` 308-redirect to the production names. The desktop,
the Go daemon, the runner, the mobile apps and browsers are the App's
clients.

Public, user-facing descriptions of the same system live in `www/docs/`
(published at forkmesh.com/docs/library); the note at the top of
`www/docs/README.md` lists the pages there that still describe the Cloudflare
deployment. This page is the developer home for the App's internals.

## Code map

| Path | Responsibility |
| --- | --- |
| `app/src/app_runtime.py` | The `Default` WorkerEntrypoint: `fetch`, `scheduled`, `queue`; the stateless fast paths and the handoff to `entry.py` |
| `app/src/entry.py` | The router and most route handlers (~50k lines), the Durable Object classes `ForkMeshRoom`, `ForkMeshNodes`, `ForkMeshNoteRoom`, `ForkMeshCronRunner`, `ForkMeshCronWatchdog`, `ForkMeshDiscordGate`, crypto helpers, `ensure_schema` |
| `app/src/*.py` (other modules) | Domain modules loaded on demand through `_LazyModule` (for example `plan_services`, `patreon`, `repository_webhooks`, `public_snapshots`, `d1_read_guard`, `activitypub`, `git_http`, `hosted_git`, `apns`, `remote_mcp`) |
| `app/src/turnstile.py` | Cloudflare Turnstile check on the signed-out account forms |
| `app/src/runner_leases.py`, `app/src/runner_logins.py` | Runner claim, report, security incident and web sign-in APIs ([RUNNERS.md](../RUNNERS.md)) |
| `app/src/hosted_repos.py` | Repositories hosted on the core nodes ([HOSTED_REPOS.md](../HOSTED_REPOS.md)) |
| `app/src/marketplace_reviews.py` | Market star ratings with required improvement reviews ([MARKETPLACE_RATINGS.md](../MARKETPLACE_RATINGS.md)) |
| `app/src/apps.py`, `app/src/apps_core.py`, `app/src/apps_runtime.py` | Apps (migration 0203, [APPS.md](../APPS.md)) |
| `app/src/ecosystem.py` | ForkMesh Ecosystem: the admin console's Ecosystem section and `GET /api/ecosystem` (migration 0213, [ecosystem.md](ecosystem.md)) |
| `app/src/docs_book.py`, `app/public/dashboard/js/09-docs.js` | The `/dashboard/docs` page: every `docs/**/*.md` of forkmesh/forkmesh as one page, edited in place by admins, one commit on `main` per save ([docs-book.md](docs-book.md)) |
| `app/src/repository_file_edits.py`, `app/public/dashboard/js/05-repo-list-explorer.js`, `app/public/dashboard/js/06-repo-content.js` | Browser edits for repository file views: Edit button, text editor and signed Git push through `POST /api/repo/<owner>/<repo>/file-edit` ([repository-file-edits.md](repository-file-edits.md)) |
| `app/src/challenges.py`, `app/src/challenges_runtime.py` | Code challenges between users: `/api/challenges` and the `/dashboard/challenges` page (migration 0217, [challenges.md](challenges.md)) |
| `app/src/workflows.py`, `app/src/workflow_integrations.py`, `app/src/workflows_runtime.py` | Workflows: the visual automation builder with 100 integrations, `/api/workflows` and the `/dashboard/workflows` page (migration 0251, [workflows.md](workflows.md)) |
| `app/src/remote_forks.py` | Forks on other instances: `/api/repo/<owner>/<repo>/remote-forks`, the Fork menu's list and "Send to" in the new pull request form (migration 0252, [remote-forks.md](remote-forks.md)) |
| `app/src/repository_provider_mirror.py` | Mirroring a repository to GitHub or GitLab: `/api/repo/<owner>/<repo>/provider-mirror`, the Settings card and the Code page's Mirror button (migration 0257, [provider-mirrors.md](provider-mirrors.md)) |
| `app/src/instance_pages.py` | A white-label instance's own `/docs` and `/desktop` pages ([../INSTANCES.md](../INSTANCES.md), "Docs and desktop pages") |
| `app/public/dashboard/js/09-agents.js`, `account_desktop_agents_handler` in `app/src/route_handlers.py` | The `/dashboard/agents` page: every agent session the account can see, and `GET /api/account/desktop-agents` ([agents-page.md](agents-page.md)) |
| `app/src/smtp_mail.py` | Mail through Postfix on the node's loopback when `MAIL_TRANSPORT = "smtp"` ([MAIL_LOG.md](../MAIL_LOG.md)) |
| `app/src/schema.py` | Runtime DDL and the schema fingerprint stored in `schema_meta` |
| `world/src/*.py` | World and Office routes and the `ForkMeshWorld` / `ForkMeshOfficeRoom` Durable Objects; staged into the App |
| `world/src/world_office_tasks.py` | Organization tasks API; per-task progress is described in [task-progress.md](task-progress.md); Markdown task text (details, QA steps, completion notes, replies) in [task-markdown.md](task-markdown.md); deletion, the 30-day trash, restore and purge in [task-trash.md](task-trash.md) |
| `app/src/admin_console.py` (Users editor), `app/host/forkmesh_host/ops.py` | The early iOS tester flag and the list the Mac ad hoc publisher emails (migration 0224, [early-ios-testers.md](early-ios-testers.md)) |
| `world/src/world_task_pings.py` | Tasks Pings for every task change, coalesced per task and digested in bulk ([task-pings.md](task-pings.md)) |
| `app/src/mail_activity.py` | Every sent email of any kind (`email_sends`), with its opens, clicks and views counted and noted in #email-log ([MAIL_LOG.md](../MAIL_LOG.md), "Every email is tracked") |
| `world/src/world_task_email.py` | An email for every task change; its opens, clicks and views also go to the task timeline ([task-email.md](task-email.md)) |
| `app/src/ids_console.py` | The admin console's Intrusion detection page: node health, incident charts, groups and log over `security_incidents` (migration 0243, [../IDS.md](../IDS.md#admin-console-page)) |
| `app/src/not_found_log.py` | 404 tracking: grouped counts, hourly chart, exceptions, the 10-hit admin Ping and the console's 404s list (migrations 0239 and 0242, [not-found-tracking.md](not-found-tracking.md)) |
| `app/src/agent_guestbook.py`, `app/src/agent_notes.py`, `app/src/guestbook_runtime.py` | The public AI guestbook and anonymous agent notes with their admin approval queue (migration 0225, [agent-notes.md](agent-notes.md)) |
| `marketplace/packages/*/content/worker/*.py` | Server code of marketplace packages (for example `analytics`, `calories`, `crm`); staged flat next to the app modules |
| `app/migrations/NNNN_*.sql`, `app/migrations/down/` | Numbered migrations (roll-forward) and their down scripts; World migrations live here too. 0191 to 0199 were added for the core nodes |
| `app/wrangler.toml` | Not deployed. Still the source of the App's `[vars]`, binding lists, queue consumer settings and `run_worker_first` list: the host reads `[vars]` and the bindings (`app/host/forkmesh_host/env.py`) and `deploy/core/caddy_routes.py` reads `run_worker_first` |
| `app/host/` | The CPython host: ASGI front, bindings, rqlite D1 adapter, clustered Durable Objects, vault, request guard, IDS records |
| `app/public/` | Static App pages: login, signup, dashboard shells and fragments, chat, notes, status, ledger, referral, pitch deck ([login page](login-page.md)). `status.html`, `network.html` and `leaderboards.html` are also published by the www Worker, and production redirects them there ([www](../www/README.md)) |
| `app/public/dashboard/js/*.js` | Dashboard source fragments, concatenated into `app/public/dashboard.js` by `app/tools/build_dashboard_assets.py` |
| `app/public/dashboard/js/09-update-notice.js` | The "new version of ForkMesh is available" notice with its one-click Update ([update-notice.md](update-notice.md)) |
| `app/tools/build_worker_python.py` | Stages `app/src`, `world/src` and package worker modules into `app/src_build/` with comments and docstrings stripped; the host runs `src_build/entry.py` |
| `app/tools/build_site_assets.py` | Stages `app/dist`, `www/dist`, `world/dist` |
| `world/tools/build_worker_footprint.py` | Python source footprint report (`LAZY_MODULES`) and World static budgets; fails the build when a static budget is exceeded ([worker-footprint.md](worker-footprint.md)) |
| `app/deploy.sh`, `app/pywrangler.sh`, `app/migrate.sh` | The Cloudflare-era deploy wrapper, pinned `pywrangler`/wrangler wrapper and D1 migration runner. They do not deploy the App to the cores |
| `app/tests/`, `app/tests/host/` | App tests (many read source text; see Traps); host tests (request guard, egress, quarantine, cluster security, end to end) |
| `app/browser_tests/` | Playwright specs for dashboard, chat and public pages |

## Architecture

**Runtime.** `forkmesh_host.runtime` loads the Worker entry module unchanged
and drives `fetch`, `scheduled` and `queue`. `forkmesh_host.asgi` turns HTTP
and WebSocket requests into `Default.fetch` calls under uvicorn
(`python -m forkmesh_host serve`). Static files are served by Caddy, never by
Python; paths in `run_worker_first` go to the App.

**Bindings** (`app/host/forkmesh_host/env.py`). `DB` is rqlite through
`d1_rqlite.py`; `REPOSITORY_METADATA` is `LocalKV` on a local SQLite file on
each node; `NOTIFICATION_QUEUE` is `LocalQueue`; `ASSETS` is `StaticAssets`
on `app/dist`; `AI` exists only when `AI_API_URL` is set (an
OpenAI-compatible endpoint, `OpenAICompatibleAI` in `bindings.py`), and
production leaves it unset; Durable Object namespaces run across the cluster
(`durable_cluster.py`).

**Request path.** `app_runtime.Default.fetch` answers a few identity routes
without touching the database or a Durable Object (`/api/version`, `/health`,
the root redirect). Everything else goes through `_app_ingress_fetch` in
`entry.py`, and most requests run directly in the request that received
them. Three families are routed into one of sixteen fixed
`FORKMESH_CRON_RUNNER` ingress objects (`_app_ingress_shard`): mirror daemon
sync traffic (user agent `ForkMesh-Mirror-*`), Git clone reads (sharded by
repository), and a short list of bursty public reads and heavy routes such as
`/product-analysis` (`_serialized_http_request`). The sharding dates from the
Cloudflare deployment, where it borrowed the Durable Object CPU allowance; on
the cores the ingress objects run on every node. If that object is
unavailable the request gets a marked 503 (`EXPECTED_DEGRADED_HEADERS`,
`retry-after: 30`) instead of running the heavy work inline.

**Durable Objects.** `ForkMeshRoom` (chat rooms and relay WebSockets),
`ForkMeshNodes` (per-owner node event sockets), `ForkMeshNoteRoom` (live
notes), `ForkMeshWorld` and `ForkMeshOfficeRoom` (World presence and Office
meetings, in `world/src/world_durable_objects.py`), `ForkMeshCronRunner` (the
cron coordinator and the ingress shards), `ForkMeshCronWatchdog` (tick
completion), `ForkMeshDiscordGate` (Discord bot gateway). On the cores each
object is placed on one live node by rendezvous hash, and its storage and
alarms are kept in rqlite (`do_storage`, `do_alarms`, migration 0192).
Details: [cron-and-durable-objects.md](cron-and-durable-objects.md).

**Storage.** `DB` is the database of record for accounts, catalog,
collaboration metadata, pings and everything operational; it keeps D1's
binding name and SQL dialect. Most row payloads are AES-GCM blobs under
`DATA_KEY` with HMAC blind indexes for lookups. `REPOSITORY_METADATA` holds
repository metadata projections. Git bytes are never stored in the database;
they live on desktop nodes and in the cores' Git stores
([HOSTED_REPOS.md](../HOSTED_REPOS.md)). See [d1.md](d1.md).

**Cron and alarms** are leaderless. Each minute the host calls `scheduled`,
which rings the `scheduled-runner-v1` coordinator object; the coordinator
rotates across live nodes every minute, and alarms fire exactly once
(migration 0196, `host_cron_runs` records which node ran which minute). The
alarm runs the phases due that minute, packing them under
`CRON_BATCH_SUBREQUEST_CAP` (a budget from the Workers subrequest limit) and
chaining alarms when it runs out. The status projection chain
(`status-sample`, `status-alert-pings`, `status-alert-email`,
`snapshot-status`) runs first. See [OPERATIONS.md](../OPERATIONS.md)
section 4.

**Queue.** `NOTIFICATION_QUEUE` carries a payload-free doorbell for instant
notification email; the consumer (`Default.queue`) sends the mail, through
Postfix on the node's loopback with `MAIL_TRANSPORT = "smtp"`
([MAIL_LOG.md](../MAIL_LOG.md)). The per-minute cron is the backstop for
overflow.

**Push and event delivery.** Clients do not poll for display data (see
`www/docs/operations/polling-elimination.md`). Changes reach the desktop and
phones as frames on the node event socket (`ForkMeshNodes`) and as APNs
pushes (`app/src/apns.py`).

**Secrets.** Nodes start sealed; `DATA_KEY` and the other vaulted secrets
arrive over the unseal socket ([VAULT.md](../VAULT.md)).

## Protocols and interfaces

- HTTP API under `/api/*`. The canonical base is
  `https://app.forkmesh.com/api`; `forkmesh.com/api/*` is proxied by the
  `forkmesh-www` Worker. The endpoint catalog the desktop shows is
  `desktop/resources/network/endpoint-catalog.json`.
- Routes added for the core nodes: `/api/runners/*` (runner claims, reports,
  login relay, pool status), `/api/hosted-repositories/*` (core replica
  reconciliation), and the Market review endpoints
  ([MARKETPLACE_RATINGS.md](../MARKETPLACE_RATINGS.md)).
- The phone-to-desktop request queue `/api/repo/<o>/<r>/desktop-commands`
  ([desktop-commands.md](desktop-commands.md)), the public ecosystem directory
  `/api/ecosystem` ([ecosystem.md](ecosystem.md)), code challenges between
  users `/api/challenges` ([challenges.md](challenges.md)), the browser
  desktop-control key `/api/accounts/devices/browser-control`, which lets the
  Agents tab manage the owner's desktop agents
  ([desktop-agents-web.md](desktop-agents-web.md)) and Settings → Agents &
  runners manage its Claude Code and Codex accounts
  ([settings-agents-and-runners.md](settings-agents-and-runners.md)), the account's
  desktop-reported agent sessions `/api/account/desktop-agents`
  ([agents-page.md](agents-page.md)), and the World's compact member roster
  `/api/accounts/users?view=world` with the avatar endpoint
  `/api/accounts/<name>/avatar` ([member-roster.md](member-roster.md)).
- Unique repository views `/api/repo/<o>/<r>/view` and the batch lookup
  `/api/repo-views` ([repo-views.md](repo-views.md)).
- Today's activity shared across a member's devices
  `/api/accounts/activity-today` ([activity-today.md](activity-today.md)).
- `forkmesh-www` reaches the App over the public internet at `APP_ORIGIN`
  and vouches for the visitor with the signed `x-forkmesh-edge-client`
  header, which the host verifies (`deploy/core/edge_client.md`).
- Versioned integration contract (HTTP, events, webhooks):
  `www/docs/integrations.md`. Outbound repository webhooks
  (`forkmesh-webhook-v1`) are sent by `app/src/repository_webhooks.py`;
  owners register them at `POST /api/repo/<owner>/<repo>/webhooks`. Four
  events: `repository.push`, `branch.push`, `pull.merged`,
  `release.published`. Deliveries are queued and drained by the
  `webhook-outbox` cron phase, up to 6 attempts. Private repositories emit
  nothing.
- Git Smart HTTP: `app/src/git_http.py`, `app/src/hosted_git.py`.
- ActivityPub and NodeInfo under `/ap/*`, `/.well-known/*`, `/nodeinfo/*`,
  `/@name`: `app/src/activitypub.py`, `app/src/activitypub_threads.py`;
  design in `www/docs/design/activitypub-federation.md`.
- Remote MCP endpoint at `/mcp`: `app/src/remote_mcp.py`.
- Organization bot tokens (`fmbot_`) and the scopes each route checks:
  [organization-bot-scopes.md](organization-bot-scopes.md).
- Public snapshots: five anonymous GET routes served from validated
  `public_api_snapshots` rows (`app/src/public_snapshots.py`); contract in
  `www/docs/operations/worker-split.md`.
- Product analytics ingest at `/api/analytics/*` from the `analytics`
  marketplace package; see [analytics.md](analytics.md).

## Configuration

- Non-secret vars: `[vars]` in `app/wrangler.toml` (origins, `NODE_NAME`,
  `WORKER_ROLE`, `SINGLE_WORKER_SITE`, `RUNNER_POOL_TARGET_NODES`,
  `HOSTED_GIT_CORE_NODES`, `MAIL_TRANSPORT`, attribution allowlists,
  `PATREON_OAUTH_REDIRECT_URI`, Discord bug-watcher ids, the public
  `TURNSTILE_SITE_KEY`). Optional overrides are listed as comments in
  `app/wrangler.toml` instead of being bound, with the built-in default.
- Overrides, in increasing precedence: an env file (`--env-file` /
  `FORKMESH_ENV_FILE`), `FORKMESH_VAR_<NAME>` process variables, and systemd
  credentials (`env.py`). `[production.vars]` in `deploy/fleet.toml` is
  rendered into the nodes' `app.env` as `FORKMESH_VAR_*`: the production
  origins, Discord bug-watcher ids, the reward network and `AP_OUTBOUND`.
- Secrets: bootstrap credentials per node (`deploy/core/render.py`,
  `tools/next_fleet.py secrets`); everything else, `DATA_KEY` and
  `ADMIN_PASS` included, is sealed in the `host_secrets` table
  (`tools/next_vault.py`). Optional shared service credentials, such as
  `TURNSTILE_SECRET_KEY`, are listed under `optional_secrets` in
  `deploy/fleet.toml`.
- Runtime: the nodes run Debian's Python 3.13. `compatibility_date` and
  `compatibility_flags` in `app/wrangler.toml` only select the Workers
  runtime (Python 3.14 on Pyodide 314.0.6).

## Build, run, test

```sh
python3 app/tools/build_dashboard_assets.py      # after editing app/public/dashboard/js/*
python3 app/tools/build_worker_python.py         # stage app/src_build/
python3 world/tools/build_worker_footprint.py --staged   # check budgets
python3 -m pytest app/tests/host                 # host tests
python3 -m pytest -q app/tests/test_<area>.py    # one App module at a time
python3 tools/next_deploy.py build               # plan a release build (dry run)
```

To run the App locally, use `python -m forkmesh_host serve` with a SQLite
database ([docs/host/README.md](../host/README.md), "Build, run, test"). Run
the App suite in small batches of files; the whole `app/tests` suite at once
can exhaust memory. Browser tests:
`cd app/browser_tests && npm ci && npx playwright test <spec>`.

## Operations

- Releases, migrations and rolling restarts:
  [docs/deploy/README.md](../deploy/README.md) and
  [OPERATIONS.md](../OPERATIONS.md) (`tools/next_deploy.py all --execute`;
  migrations run through `tools/rqlite_migrate.py`). A node that fails its
  gate goes back to its previous release and the rollout stops. Check the
  rev at `https://app.forkmesh.com/api/version` before assuming a merged fix
  is live.
- Logs: the desktop Log page follows every node's journal
  ([NODE_LOGS.md](../NODE_LOGS.md)); on a node,
  `journalctl -u forkmesh-app`. Application errors land in the `error_log`
  table and Sentry (hand-rolled envelope, no SDK). `log_error` redacts a
  request path that could name a private repository (`/owner/repo...`
  becomes `/private-or-unpublished-repository/[kind]` with the message
  replaced). Internal labels such as `forkbot/ai`, `plugin/ai` or
  `username/moderation` have no leading slash and pass through unchanged:
  before 2026-09-25 `forkbot/ai` was read as the repository `forkbot/ai`, so
  every AI failure landed in the redacted group with its text removed. Two
  message shapes keep their class through the redaction: `response status
  NNN`, and `log_d1_unavailable`'s `d1 unavailable: <raw>`, which becomes
  `d1 unavailable (<kind>): Repository-route error details redacted.` with
  `<kind>` one of `overload`, `too many`, `internal error`, `network
  connection lost` or `other` (a fixed vocabulary, never the raw text). Before
  2026-09-28 a D1-down 503 on a private repository's `info/refs` read only
  "Repository-route error details redacted.", so it could not be told from
  any other 503 on that route (`_privacy_safe_error_text`,
  `app/tests/test_sentry_worker.py`).
- The first row of each new `error_log` group (status, method, path, message)
  pings administrators as "New … error group" (`_notify_new_error_group`).
  `log_d1_unavailable` rows do not ping (`_write_error_log(..., notify=False)`,
  since 2026-10-06). A database that is overloaded or electing a leader
  answers a retryable 503. The client backs off and the next request
  succeeds, so there is nothing to fix in code. Yet each route × kind opened a
  new group and pinged, and once the group was deleted or rolled out of the
  log, it pinged again. The ping of 2026-10-06 14:52 (`GET
  /private-or-unpublished-repository/[git-info-refs] — d1 unavailable
  (overload)`) was one of these. The rows are still written, so /status and
  `/admin?table=error_log` still count database unavailability
  (`app/tests/test_error_group_notifications.py`).
- Web Requests statistics are counted by the desktop (`WebRequestTally` in
  `desktop/src/ControlNode.cpp`) from each core's Caddy access log, read
  through the node log reader ([desktop networking](../desktop/networking.md),
  Fleet node logs). A core's `web-requests` journal lines
  (`app/host/forkmesh_host/request_tally.py`) are only the fallback while
  that core has no access log. Nothing is stored in the database.
- Cron costs per tick: `GET /api/accounts/admin-cron-report`, or the
  desktop Log page's Cron tab.

## Security

- The database is the Cloudflare deployment's D1 data, imported into rqlite
  and re-keyed onto the cores' own `DATA_KEY` on 2026-09-24
  ([REKEY.md](../REKEY.md)). `DATA_KEY` encrypts row payloads; blind indexes
  are HMACs. There is no key-version byte yet, so `DATA_KEY` cannot be
  rotated in place; it is rotated only offline with the apps stopped
  (`tools/next_rekey.py`).
- The host applies a Host allowlist, streamed body caps, an SSRF egress
  guard and edge-client verification before the App sees a request
  (`request_guard.py`, `egress.py`).
- Signup forms require Turnstile (`app/src/turnstile.py`).
- Every failed signup pings the administrators, without the visitor's email
  ([signup-failure-pings.md](signup-failure-pings.md)).
- Account sessions: see [auth-sessions.md](auth-sessions.md).
- The admin console is behind `ADMIN_PATH` and `ADMIN_PASS` with a 12-hour
  `forkmesh_admin` cookie. Its Database tab hides and write-blocks the host
  tables such as `host_secrets` (d1a577f).
- ActivityPub delivery is off by default on the host and turned on by
  `AP_OUTBOUND` in `[production.vars]` (d1a577f).
- Webhook destinations must be public routable https URLs, never
  forkmesh.com or its subdomains (checked again at send time); the URL and
  secret are stored encrypted.
- Payments: no card data reaches the App; Patreon is the only payment rail
  ([payments-and-plans.md](payments-and-plans.md)).

## Traps

- **`app/wrangler.toml` still looks like a Cloudflare config.** Only its
  `[vars]`, binding lists, queue consumer settings and `run_worker_first`
  matter to the cores; edits to routes, D1 or KV ids, or compatibility flags
  do nothing there.
- **Python versions differ.** Nodes run Debian's Python 3.13, not the 3.14
  the Workers runtime used.
- **Lazy modules.** Startup source is `entry.py`, `app_runtime.py`,
  `world_durable_objects.py` and the vendored SDK. A new module imported
  through `_LazyModule` belongs in `LAZY_MODULES` in
  `world/tools/build_worker_footprint.py` so the footprint report shows it as
  on-demand. The Cloudflare startup budget (error 10021) that used to fail
  deploys was retired on 2026-09-30. See
  [worker-footprint.md](worker-footprint.md).
- **Served line numbers do not match `app/src`.** The host runs the
  compacted `app/src_build/`; regenerate it with
  `python3 app/tools/build_worker_python.py` to read a traceback.
- **Stale `dashboard.js`.** Edits under `app/public/dashboard/js/` do
  nothing until `app/tools/build_dashboard_assets.py` rebuilds the bundle.
- **One stuck digest queue stops every fediverse digest.** The `ap-digest`
  phase publishes `AP_DIGEST_CRON_BATCH` (1) queue per run, oldest
  `next_ts` first. A queue whose repository actor has no `ap_actors` row
  used to return without moving `next_ts`, so it stayed first forever. The
  core-node scope of an org-backed repository (`fmnext-n1/forkmesh` behind
  `forkmesh/forkmesh`) is such a queue: nobody looks it up, so no actor is
  minted. From 2026-09-26 it held back the `forkmesh/forkmesh` digest and
  its 4 followers; the last post before that was on 2026-09-17. A queue
  without a local actor is now deleted, like a private or revoked scope; an
  actor with no row has no followers to deliver to. Until that code was
  deployed, the stuck queue's `next_ts` was moved out by hand (2026-09-28
  01:23 UTC, then 30 days out at 01:39); the 01:28 run published the held
  digest and delivered it to 3 inboxes.
- **Source-text tests.** Many tests in `app/tests` read `entry.py` as text
  and slice functions; moving a function between modules can break a test
  without changing behaviour.
- **Schema fingerprint.** Any edit to `app/src/schema.py` invalidates the
  `schema_meta` fingerprint; the next cold start replays DDL once
  (single-flighted in `ensure_schema`).
- **Migrations are roll-forward.** A rollback of code must still accept the
  newest schema. The Cloudflare deployment's
  `0191_org_agent_attachments.sql` was ported as `0201`; see
  [NEXT_ADOPTION.md](../NEXT_ADOPTION.md) and [CUTOVER.md](../CUTOVER.md)
  before porting a migration. `app/migrate.sh` refused any migration that
  deletes the ActivityPub follower graph; `tools/rqlite_migrate.py` has no
  such check.
- **rqlite freezes `RANDOM()` and `'now'` in triggers and views.**
  `tools/rqlite_migrate.py` sends such files without the rewrite
  (`app/host/RQLITE_FINDINGS.md`).
- **A local rqlite read can miss a write** that just committed elsewhere;
  the adapter confirms misses with the leader (0adec3d, `d1_rqlite.py`).
- Organization repositories (`forkmesh/forkmesh` is one) reach route handlers
  with `owner` already rewritten to the backing node (`org_alias_rewrite`).
  Anything public, such as an ActivityPub actor handle, note text or link,
  must use the organization name from the original URL
  (`_ap_public_repo_owner`), while privacy, settings and repository rows use
  the node. Stored ActivityPub notes keep the organization name in their
  context, so readers resolve it back to the node (`_ap_org_alias_owner`)
  before checking privacy.
- Every action in `REPO_HOST_RE` (`app/src/urls.py`) must be in
  `REPO_HOST_READ_ACTIONS` (`app/src/app_runtime.py`); anything else that
  matches the regex falls into the retired-socket branch and answers the
  cacheable 410 `repository_socket_transport_retired`, which clients keep for
  a day. `host` is the only action meant to land there.
  `app/tests/test_repo_read_route_operations.py` fails when the two drift, and
  also checks each served read against `edge_routing.PUBLIC_OPERATIONS`, the
  gateway's default and direct public reads, the Go node's
  `defaultGatewayOperations`, `hosted_git.HOSTED_API_READ_OPERATIONS` and the
  host fast path's `READ_OPERATIONS`.
- **Read guard.** Heavy readers go through `app/src/d1_read_guard.py`
  `shared_read`, built for D1's rows-read budget, where per-isolate memos did
  not bound the bill. `tools/rqlite_migrate.py` sets the `d1_read_guard`
  switch `off` by default, since rqlite has no read quota.

## Recent changes

- 2026-10-13: Chat pushes carry the relay `ref` and key version of the
  stored ciphertext, and the phones fetch it from new read-only
  `GET /api/chat/direct-messages/<id>/pending` and
  `GET /api/chat/channels/<id>/pending` routes and decrypt it on the device,
  so a banner shows the sender and the text while the server still sees
  neither, the way Signal does. Direct message pushes are no longer collapsed
  or coalesced ([chat-push-previews.md](chat-push-previews.md)).
- 2026-10-13: Every own chat message shows a sending, sent, delivered or read
  mark on every client, and "Delete for everyone" in a direct message also
  withdraws the server copy with `forkmesh-superseded`
  ([chat-private-rooms.md](chat-private-rooms.md#delivery-ticks-on-every-own-message-all-clients-2026-10-13),
  [delete for everyone](chat-private-rooms.md#delete-for-everyone-replies-and-reactions-in-direct-messages-2026-10-13)).
- 2026-10-13: A repository can be mirrored to GitHub or GitLab with the
  owner's access token, from the Settings tab or the Code page's Mirror
  button. Code and releases are pushed out; issues, pull requests and
  discussions can go either way, imported automatically or picked by hand
  ([provider-mirrors.md](provider-mirrors.md)).
- 2026-10-13: Signup renders Turnstile explicitly from `signup.js`, so a
  solved check can no longer leave the form stuck on "Complete the security
  check". Every signup failure, in the server or the browser, now pings the
  administrators ([signup-failure-pings.md](signup-failure-pings.md)).
- 2026-10-12: Office chat now holds the account event channel, so read marks
  and new DMs reach it live, and the site header's chat icon clears when
  `#general` was read on another device (`readAt` on `GET /api/chat/reads`;
  [chat-read-sync.md](chat-read-sync.md#site-header-chat-icon)).
- 2026-10-09: Reading a chat on one device clears its unread count on your
  other devices. Clients save per-conversation read marks with
  `/api/chat/reads` (table `chat_read_marks`, migration 0255), and the silent
  `chat-read` account event tells the other devices to fetch them
  ([chat-read-sync.md](chat-read-sync.md)).
- 2026-10-12: Pull request and issue alerts name the account, not the key
  (issue #1907). A client that signs a pull request, review, comment or issue
  event without an `authorName` left the owner's desktop to fall back to the
  first eight characters of the signing key ("MXcj4dF_ opened a pull
  request"). After the signature verifies, `pulls_handler` and
  `issues_handler` now call `_fill_signer_author_name`, which resolves the
  signer through `_git_push_account_for_pubkey` (the account's primary key
  or an enabled device key) and stores that username as `authorName` in the
  inbox row. `authorName` is not part of any signed canonical string, so the
  signature still verifies on the receiving node. A name the client signed
  with is kept, and a key that belongs to no account keeps the old fallback.
- 2026-10-10: Core features as plugins on the web dashboard
  ([core-plugins](../marketplace/core-plugins.md) is the home page; the
  gate itself is in [docs/www/README.md](../www/README.md)). Entry points
  carry `data-plugin-entry="<package-id>"` and disappear for a signed-in
  account without that package: the sidebar's Agents, World, Chat, Network
  and Financial ledger links (partial `dashboard/partials/sidebar.html`,
  composed into every page by `app/tools/build_dashboard_assets.py`; the
  hand-written `dashboard/{tasks,notes,calendar}/index.html` copies tag
  Tasks, Calendar, Notes, Network, Chat and Financial ledger), the Network
  tab on the profile pages, the Agents list on Home, and on a repository
  the Code, Issues, Pull requests (`pull-requests`), Discussions, Actions
  and Agents tabs, the commit History link (`commits`), the Releases link
  (`releases`) and the branch menu's "View all branches" (`branches`). Links
  inside sentences are left alone. The Apps page (`/dashboard/apps`) has a
  new **Plugins** panel listing every account-synced package from
  `GET /api/market/packages` that `GET /api/plugins/@account` knows, each
  under its first dependency (Status Page checks under Status Page, Pull
  requests under Branches, Special Agents under Agents), with **Install** /
  **Remove** calling `POST /api/plugins/@account/<id>/install|uninstall`.
  Removing asks first and names every installed plugin that goes with it;
  the result updates the gate (`rememberPlugin`, then one
  `refreshPlugins(true)`) and fires `forkmesh:plugins-changed`, which
  redraws the Plugins column. The panel loads on open and on **Refresh**
  only; it never polls.
- 2026-10-10: Forks on other instances. A repository's Fork menu saves the
  addresses of its copies on other ForkMesh instances, and the new pull
  request form can send a signed pull request to one of them; the target's
  pulls endpoint accepts that POST from any origin without credentials
  ([remote-forks.md](remote-forks.md)). On white-label instances the blog
  card, the Genie spider and the status page became plugins that start off,
  and `/docs`, `/desktop` and `/@desktop` got instance pages instead of 404s
  ([../INSTANCES.md](../INSTANCES.md)).
- 2026-10-09: Signal-style delivery ticks on your own direct messages in web
  `/chat`: sending, sent, delivered (two rings) and read (two filled dots),
  driven by new `forkmesh-delivered` and `forkmesh-read` relay frames
  ([chat-private-rooms.md](chat-private-rooms.md#delivery-ticks-on-direct-messages-2026-10-09)).
- 2026-10-09: No toasts on the dashboard. The user asked for no toasts
  anywhere, with the information shown inline where it is appropriate, so
  nothing reports itself in a floating card any more:
  - **Tasks** (`09-tasks.js`): each result appears as a status line in the
    part of the task it concerns: under the action bar (start, stop, complete,
    QA, created, updated, check-in sent, reminder sent, prompt copied), under
    the comment box, in the Progress, Persistent reminder, Check in and Ask for
    a check-in cards. Errors use the destructive colour and stay until the next
    action or until another task is selected. Trashing a task puts "Task moved
    to trash. Undo" above the task list ([task-trash.md](task-trash.md#clients)).
    `showTaskToast` / `showTaskUndoToast` became `setTaskNotice` /
    `setTaskListNotice`; the slots are `[data-task-notice="<place>"]` and
    `[data-tasks-notice]`.
  - **Notes** (`09-notes.js`): `noteToast` became `noteNotice(place, ...)`. A
    line under the error banner reports note-level results (deleted, not
    found, could not create); link results show under the linked-conversation
    chips, sharing, publishing and Copy link inside the Share popover, and a
    version restore in the Version history drawer. Choosing another note or
    starting a new one clears it.
  - **Code map** (`06-repo-content.js`): "no size data yet" and "couldn't open"
    show as a line under the repository toolbar that holds the Code map
    button, in place of a pill at the bottom of the window.
  - **Update notice**: the "new version is available" card is now a strip at
    the top of the main column ([update-notice.md](update-notice.md)).
  The hand-maintained `dashboard/tasks/index.html` and the Tasks and Notes
  package views carry the same markup. Tests:
  `test_task_feedback_is_inline_next_to_the_action_not_a_toast` in
  `app/tests/test_dashboard_tasks_management.py` and the task comment and
  progress browser specs, which now read the inline lines.
- 2026-10-09: Workflows, a visual automation builder in the style of n8n:
  a canvas of triggers (manual, webhook, schedule), logic steps and 100
  integrations, with encrypted credentials and execution history
  ([workflows.md](workflows.md)).
- 2026-10-08: Direct messages that the recipient's device could not open
  (sealed before the sender knew its key) stayed on the relay for 7 days.
  Clients now keep key advertisements on the relay, ask for a re-seal, and
  re-seal their own undelivered messages, which replace the old copies through
  `forkmesh-superseded`. Only real messages raise the DM unread count and push
  ([chat-private-rooms.md](chat-private-rooms.md#undeliverable-copies-and-re-sealing-2026-10-08)).
- 2026-10-07: The dashboard notices a new deploy (`/api/version` `rev`) and
  offers a one-click Update that reloads the page
  ([update-notice.md](update-notice.md)).
- 2026-10-07: Every 404 on the App and on forkmesh.com is counted per
  (site, method, path) in `not_found_paths` (migration 0239) and listed in
  the admin console under Reliability, "404s". A group that reaches 10 hits
  sends administrators one Ping, at most five per hour
  ([not-found-tracking.md](not-found-tracking.md)).
- 2026-10-07: The 404s page charts hits per hour over the last 24 hours,
  overall and per group (`not_found_hours`, migration 0242), and takes
  exceptions (`not_found_ignores`): an ignored path, or everything under it,
  is no longer logged, for things deleted on purpose
  ([not-found-tracking.md](not-found-tracking.md)).
- 2026-10-07: The admin console has a Security section, "Intrusion
  detection": IDS heartbeat and quarantine state per node, incident counts
  against the previous window, a chart over 24 hours, 7 or 30 days, incidents
  grouped by detector or node with sparklines, and the incident log with
  severity, node and detector filters (read-only;
  [../IDS.md](../IDS.md#admin-console-page)).
- 2026-10-07: Chat messages use one Slack-style syntax (`*bold*`, `_italic_`,
  `~strike~`, code, code blocks, quotes, lists, `<url|label>` links) on the
  web, the desktop and in notification emails. The web composer writes the
  Slack markers and takes Slack's shortcuts, `app/src/chat_format.py` renders
  notification email bodies, and the unread-channel reminder lists its
  channels in one table ([chat-formatting.md](chat-formatting.md)).
- 2026-10-06: `/api/world/deploy-status` on the stateless fast path
  (`_stateless_fast_response` in `app/src/app_runtime.py`) answers a database
  outage (`_is_d1_platform_error`) with a 503 `database_unavailable`, marked
  expected-degraded with `Retry-After: 2` and logged by `log_d1_unavailable`,
  the same as the full request path, instead of raising out of the fast path.
- 2026-10-06: Today's activity chart syncs across desktop, iOS, Android and
  the web dashboard. Each device sends its five-minute counts to
  `/api/accounts/activity-today` (table `account_activity_days`, migration
  0235), and every chart adds the other devices' counts. The web dashboard
  now counts input and shows the chart on a merged agent session
  ([activity-today.md](activity-today.md)).
- 2026-10-06: ForkMesh counts unique views of public repositories (one per
  viewer per 24 hours; tables `repo_views` and `repo_view_totals`, migration
  0234). The Home "Top repositories" rail lists the most recently viewed
  repositories first and shows each one's view count
  ([repo-views.md](repo-views.md)).
- 2026-10-04: The Branches page no longer stops at "This repository's node
  has not taken the branch operations already queued for it" when the
  relay's per-repository queue (`MAX_PENDING_BRANCH_OPS`, 1024) is full. A
  request answered `queue_full` is kept in a list on the page, its branches
  shown as "deletion waiting", "update waiting" and so on (✕ drops one). The
  page sends at once as many as the 429's `room` allows. Every 20 seconds it
  rereads the queue and sends as many more as now fit, until the list is
  empty. Every `/branches/ops` answer, the 429 included, now reports `limit`,
  and `orgApiRequest` errors carry the response body as `error.data`. A
  network or server error keeps the list for the next round. A refusal (not
  authorized, a bad name or commit) drops the batch it was about and says
  why. The list lives in the open page, so closing it drops what was not sent
  yet. Single operations behave the same, so one delete or merge made while
  the queue is full waits too.
- 2026-10-02: Task reminders always reach the assignee and are tracked. They
  are now delivered even when the assignee muted Mentions or paused pings.
  Each reminder adds a `reminded` timeline entry that says what was delivered.
  The task shows when the assignee saw it, by opening the ping or the task
  (`lastReminder.seenAt`/`seenVia`, plus a `reminder_seen` entry). Migration
  0230 rebuilds `organization_task_events` for the two new kinds. See
  [task-pings.md](task-pings.md), "Check-in reminders".
- 2026-10-01: Task check-in reminders are emailed right away. A reminder is a
  `mention` ping with no issue, pull or discussion source, so a digest-mode
  assignee used to read it in the hourly rollup. `notification_email_immediate`
  now also counts a ping whose `meta.context` is in
  `NOTIFICATION_IMMEDIATE_CONTEXTS` (`reminder`), so the queue consumer mails
  it on the immediate rail. The recipient's Mentions email setting still
  decides whether it is sent. Task @mentions (`meta.context` `comment`,
  `check-in`, `task`) still go in the digest.
- 2026-10-01: in web chat's member list and direct-message rows the presence
  dot now sits to the left of the avatar, with no ring, instead of overlapping
  the avatar's bottom-right corner inside a white (surface-colored) ring
  (`app/public/chat.css`, `.chat-person-face` / `.chat-direct-face`).
- 2026-10-01: `/status` no longer has the "Installers: Linux, Android,
  Windows & macOS" row; the four per-platform release rows cover it. The
  `probe-installer` cron phase and `_installer_delivery_probe` are gone; see
  [public-pages.md](public-pages.md), "Status".
- 2026-10-01: `/status` splits the release version check into one row per
  platform (Linux, Android, Windows, macOS) in place of "Homepage downloads",
  so each platform can be released on its own; see
  [public-pages.md](public-pages.md), "Status".
- 2026-09-30: ChatGPT can reach the AI guestbook again, and agents can leave
  anonymous notes at `GET /api/agent-notes` that wait in an is_admin-only
  approval queue (Support, "Agent notes"). The guestbook had answered ChatGPT
  403 since the cores began enforcing the user-agent block rules on
  2026-09-28 ([agent-notes.md](agent-notes.md)).
- 2026-09-30: `GET /api/repo/<o>/<r>/merge-preview` answers again. It had
  answered 410 `repository_socket_transport_retired` since it was added
  (aa5fa51a9, 2026-08-10): that change put `merge-preview` in `REPO_HOST_RE`,
  the router query builder and the gateway, but not in the router's inline
  set of HTTPS-mirror reads, so a public request fell through to the retired
  `/host` socket answer. Internal callers (`agent_branches.py`) reach the
  gateway through `_routed_repository_read` and were unaffected, which hid it.
  The phones' pull-request merge plan (`mergePreview` in ForkMeshCore
  `ApiService+Agents.swift` and Android `ReviewSession.kt`), their Conflicts
  screens, and the web Branches merge dialog and compare-page merge panel all
  read it. The set is now `REPO_HOST_READ_ACTIONS`, and `merge-preview` is
  also a private core-hosted read (`HOSTED_API_READ_OPERATIONS`) and a host
  fast-path read ([host/README.md](../host/README.md)). No other action in the
  regex was affected. Tests: `test_repo_read_route_operations.py`,
  `app/tests/host/test_repo_read_routes.py` (drives the real router in
  process), and phone-shape checks in `test_mirror_gateway.py` and
  `host/test_repo_fastpath.py`.
- 2026-09-29: `/dashboard/docs` ("Developer docs" in the sidebar) shows every
  Markdown file under `docs/` as one page. A separator line above each file
  names it and gives its last-updated date. Admins get a pencil on each
  separator to edit the file in place, and a menu on text selection to edit,
  or to delete the selected blocks or the whole section. Each save is a commit
  on `main`, pushed through the same path as `git push` (the gateway's tree
  entries now carry `mode` and `oid`). See [docs-book.md](docs-book.md).
- 2026-09-29: Deleting a task moves it to a 30-day trash
  (`DELETE /api/tasks/<id>`, batch `POST /api/tasks/delete`, restore
  `POST /api/tasks/<id>/restore`, `GET /api/tasks?status=deleted`); the hourly
  `sensitive-cleanup` phase purges it afterwards. Creator, assignee and
  managers may delete; a running agent job or pending QA needs an owner's
  `force`. The Tasks page has a Trash filter and an Undo offer, and the MCP
  server has `delete_task` and `restore_task`. Migration 0222. See
  [task-trash.md](task-trash.md).
- 2026-09-29: The World lobby's feedback kiosk, task bounty desk, Link Lab
  and link rewards board moved to the dashboard: a "Send feedback" dialog
  (account menu, sidebar footer, `?feedback=1`), `/dashboard/bounties` and
  `/dashboard/links`. `POST /api/feedback` accepts a `web` source. See
  [feedback-bounties-links.md](feedback-bounties-links.md).
- 2026-09-29: `POST /api/accounts/link-install` gives a signed-in session a
  fresh installer link code, so a node installed from the World panel joins
  the account without the desktop. See
  [release/self-setup-node.md](../release/self-setup-node.md).
- 2026-09-29: The World loads a compact roster,
  `GET /api/accounts/users?view=world` (149 KB instead of 1.47 MB; 30 KB
  gzip): no inline `avatarPng`, a 128×128 WebP `avatarThumb` made with Pillow
  (now in `app/host/requirements.txt`) and an `avatarUrl` to the new
  `GET /api/accounts/<name>/avatar?v=<hash>[&size=128]`. The default roster
  response is unchanged. See [member-roster.md](member-roster.md), including
  the privacy audit behind its cache headers.
- 2026-09-29: Analytics is default-on and listed in the dashboard's Plugins
  column (`chart-line` icon) for every member of an organization, before any
  organization installs it: `plugin_data._visible` now honours
  `install.default` for organization-scoped packages when the account belongs
  to an organization. The page offers owners and admins the install. See
  [analytics.md](analytics.md#docs-view-counter-and-default-install-2026-09-29).
- 2026-10-03: a tap on a sealed chat push now opens the conversation it was
  about. Reported as "I got an encrypted chat push, clicked it, and it did not
  go to the chat or show the message". The payload had no conversation, so the
  iPhone guessed from its unread list, which is still empty while the app wakes
  and reconnects. The guess also looked only for `@...` direct conversations,
  while server-backed direct messages are `direct:<id>`, and office channel
  unread is not in that list at all, so those taps left Chat on whatever was
  open last. `chat` and `direct-messages` payloads now carry a top-level
  `conversation` (`apns.alert_payload`, only `#...` or `direct:...`, at most
  120 characters): `direct:<conversation id>` for a direct message,
  `#office/<channel name>` (the name cut to 32 characters, as the phone does)
  for a channel, `#mesh` for a mesh wake. World chat sends none, because the
  channel is inside the ciphertext; the phone falls back to its unread guess.
  The id is random and the channel name is already in the title, so Apple
  learns nothing new. On iOS, `ApplePushBridge.openFields` copies the field
  into `PushOpen.conversation`, `pushChatConversation` prefers it and now also
  matches `direct:` in the fallback, and `ChatHub.select` reloads the direct
  or office list before opening a conversation it does not know yet. Android
  is not changed: it raises its own direct-message notifications from the
  account socket, and its tap still guesses from the relay's unread list.
- 2026-09-29: a direct-message APNs payload names its sender in a top-level
  `sender` field (a lowercase account name, dropped if it is not one), so the
  iOS notification service extension can fetch that account's avatar from the
  public `GET /api/accounts/<sender>` and attach it. Room and channel pushes
  carry no sender: the relay only knows the sender's blind index there.
- 2026-09-28: Chat message retention is a per-user preference shared by the
  web, desktop, Android and iOS clients. `POST /api/accounts/chat-retention`
  (`account_status.py`, session-authenticated like `user-status`) reads it
  with an empty body, sets the default with `{"defaultMs": ms|null}` and one
  chat with `{"chat": key, "retentionMs": ms|null}`; null clears. The answer
  is `chatRetention: {defaultMs, chats, updatedAt}`, which the owner's
  account payload (`_account_public_payload`) also carries. It is stored in
  the encrypted account record (`chat_retention`), never in the public
  profile or directory, so no migration was needed. Every value is clamped
  to between one hour and seven days: seven days is the default and the
  most, because the relay's `chat_history` keeps nothing longer, so the
  preference can only make messages disappear sooner. Overrides are capped
  at 500 per account. Chat keys: `#name` for named rooms, `channel:<id>` for
  server chat channels (the web's `private:<id>`, office channels on the
  apps), `direct:<id>` for web DMs and `@<peer>` for relay DMs. On the web,
  `chat-retention.js` holds the model (cached in
  `localStorage["forkmesh.chat.retention"]`); `chat.js` drops expired
  replays in `renderChatEntry`, prunes shown messages every minute and after
  a change, and adds a "Messages disappear after" picker to the conversation
  header with this chat's time and the default for all chats.
  `dashboard-chat.js` applies the same preference to its room, so
  `chat-retention.js` joins the App assets the World origin serves
  (`world/worker.js`, `world/wrangler.toml`, `deploy/core/caddy_routes.py`,
  `tools/cloudflare_bootstrap.py`). Tests: `test_account_chat_retention.py`,
  `test_chat_retention_frontend.py`.
- 2026-09-28 (issue #706): Tasks have a **Remind** button. Any member can
  ping a task's assignee to check in, with an optional note, at most once
  per task every 10 minutes (`POST /api/tasks/<id>/remind`). See
  [task-pings.md](task-pings.md), "Check-in reminders".
- 2026-09-28: Issue #729. The issue page's compact title bar, which slides in
  once the full header scrolls away, now sits below the dashboard header
  instead of half under its menu buttons. Its sticky holder is zero-height
  and centred the bar on its top edge (`items-center`), so 19px of the bar
  rode up under the header; it now aligns the bar to that edge
  (`items-start`, `06-repo-content.js`). On the chat page the Prompt pill
  used to sit a fixed 176px up and covered the chat composer's repository
  and task-routing selects whenever the composer grew taller (narrow phones
  wrap its toolbar, team tasks add four selects, the input resizes); the
  pill now follows the composer's measured top (see
  [web-composer](web-composer.md)).
- 2026-09-28: ActivityPub fixes for organization repositories, found while
  announcing the Open Collective blog post from `@forkmesh.forkmesh`.
  - Every `/ap/o/<uuid>` note and its `/media/<n>` images answered 404. The
    privacy check ran against the organization name stored in the note's
    context, which has no repository row, so it failed closed. Mastodon still
    showed the posts it had been sent, but boosts, URL searches and reply
    lookups on other servers could not fetch them. Both handlers now resolve
    the organization to its node first.
  - A manual `POST /api/repo/<org>/<repo>/ap-publish` built the actor handle,
    text, link and context from the rewritten node name. The actor
    `<node>.<repo>` does not exist, so nothing was sent while the endpoint
    still answered 202. `ap-posts` (list and delete) looked up the same wrong
    actor. Both now use the organization name, but only when the durable
    `org_repos` link still points at that node.
  - New manual-only `announcement` kind for `ap-publish`. The note is the
    title, the body and a `url` on this site's origin (anything else is 400
    `invalid_url`), with no "Update in owner/repo" label or author line. The
    `url` is also the note's link, so a blog post announced this way points
    at the post instead of the repository page. The daily digest never
    queues it.
- 2026-09-28 (issue #730): Issue avatars on the web show the account's
  picture and link to its profile. `issueActorAvatarHtml` (`02-helpers.js`)
  renders an `<a href="/@name">` for any label that is a valid node name
  (agent labels such as `CC`, public keys and `unknown` stay plain) and tags
  it `data-issue-actor-avatar`; a `MutationObserver`, started on first use,
  fills the picture in from `GET /api/accounts/<name>`, once per name per page
  load (private profiles stay an initial). It does not use the
  `GET /api/accounts/users` directory the ledger and World use: that answer
  carries every member's picture and is `no-store`, far more than the handful
  of people on one issue. Issue
  rows and the assignee picker pass `link = false`, because they are already
  an `<a>` or a `<button>` and cannot nest another link.
- 2026-09-28 (issue #730): A screenshot pasted or attached in the web
  new-issue form shows at once as a preview tile instead of opening the
  crop/compress dialog. Anything over the embedded-image budget (40 KB) is
  first re-encoded automatically (`fitImageWithinBytes`, capped at 1024 px by
  `ISSUE_IMAGE_FIT_OPTIONS`); the dialog opens only if that cannot fit.
  Hovering a tile (always visible on touch screens) offers Mark up and
  Remove. Mark up (`mountImageMarkupEditor`, `03-issues-profile-io.js`) opens
  an inline editor with draw, box and arrow tools, six colors, undo (Ctrl+Z),
  clear, Esc to cancel and Ctrl+Enter to finish; shapes are kept in
  normalized coordinates and redrawn over the original file, so marking up
  again can undo earlier marks without compounding compression. Encoding
  uses `canvas.toDataURL` rather than `toBlob`: Chromium runs `toBlob` in idle
  time and took about a second per attempt while the page was busy, which
  made one mark-up take over 10 s; the resize dialog shares the same
  `encodeImageWithinBytes` loop.
- 2026-09-28: Private rooms are only the creator's and the invited members'
  ([chat-private-rooms.md](chat-private-rooms.md)). Administrators lost their
  implicit access to private channels (list, room-access, history, members,
  settings, sockets, Office socket), every non-member gets the same 404 as a
  missing channel, and admins are no longer silently dropped from `members`.
  The desktop can now create rooms, manage members and use server DMs through
  new resource-bound key proofs. The web chat's people pane shows only the
  room's members in a private room or DM, the create dialog states who can see
  the room, and Message is offered from @mentions, profiles and `?dm=`.
- 2026-09-28: Ticking Viewed or the fold chevron on a pull or compare diff
  keeps that file's header where it was on screen
  (`keepPullDiffHeaderInPlace`, `06-repo-content.js`). The header is sticky,
  so a click made while reading inside a long file used to fold its body out
  from above the viewport and drop the reader somewhere in the files below,
  a distance that depended on the folded file's length. The handler now
  measures the header before and after the fold and scrolls the nearest
  scrolling ancestor by the difference.
  Follow-up the same day: two browser mechanisms undid that on some folds.
  The file block carries `content-visibility:auto`, so folding the last file
  from its sticky header at the bottom of the page left its now tiny real box
  out of view, Chrome skipped it and kept its remembered full height: the
  reader saw an empty bordered box instead of a folded header. And after a
  reopen, CSS scroll anchoring re-adjusted the scroller one frame later
  (193 px in a headless run), moving the header away from where the fold had
  left it. The handler now pins the block to `content-visibility:visible` and
  the scroller to `overflow-anchor:none` across the toggle and hands both back
  two animation frames later, once the header is on screen and the handler's
  own correction has landed.
- 2026-09-28: The dashboard's Plugins column is hidden from signed-out
  visitors and lists only what the account can use. The build renders the
  group and every link hidden, each link tagged `data-plugin-nav="<package>"`;
  once a session loads, `renderPluginNavigation` (`04-account.js`) asks
  `GET /api/plugins` (`plugin_data._visible`) and shows the returned
  packages. A package is listed when it is installed for one of the account's
  organizations (`org_plugin_installs` joined with `org_members`), when an
  account-scoped package (Calories, Vault) answers its `account_installed`
  hook for the account, or, for packages without organization or account
  data (Tasks, Calendar, Notes), when the manifest's `install.default` is
  true (`default` in the generated registry). The Browser extension page is
  therefore only listed once an organization installs it. The column
  disappears entirely when nothing is visible.
- 2026-10-02: An organization package is also listed for an account outside
  every organization that installed it when the package's
  `account_shared_access(runtime, account_bi, account_name)` hook says
  something is shared with that account. The CRM answers it from
  `crm_view_shares` (the same check as `GET /api/crm/-/shared`), so someone
  a CRM sheet was shared with by name sees the CRM link in the dashboard and
  loses it again when the share or the install is removed.
- 2026-09-28: The dashboard menu is no longer a long vertical drawer. The
  hamburger opens the same horizontal mega menu as the site header
  (`www/public/site-header.js`), and both carry the same links in the same
  groups: Dashboard (with a Home link to forkmesh.com), Community, Product,
  Resources, Company, plus Security / Privacy / Terms. The dashboard adds
  its Plugins column (`app/tools/build_app_plugins.py` `_navigation`), the
  admin-only Runners link and the Top repositories strip. The markup keeps
  the `data-dashboard-sidebar`, `data-sidebar-main-menu` and `data-nav`
  hooks, so the build-time active link and the open/close JS are unchanged.
- 2026-09-28: `/status` has one monitor per core node (`core:n1`, ...),
  built dynamically from `cluster_members`. Each checks the heartbeat, the
  active status, the peers' HTTPS readiness probes and the IDS, and
  administrators are pinged every five minutes while any core is not fully
  healthy (see [public-pages.md](public-pages.md), "Status").
- 2026-09-28: Every `/status` row shows a small pie and a countdown to its
  next check, with the exact time on hover (`nextCheck` in `/api/status`,
  computed from `_cron_due_phases`; see [public-pages.md](public-pages.md),
  "Status").
- 2026-09-28: The installer row ("Installers: Linux, Android, Windows &
  macOS", formerly "Android APK & Linux AppImage") requires the Windows
  installer (`forkmesh-windows-x86_64-setup.exe`) and the Apple silicon DMG
  (`forkmesh-macos-arm64.dmg`) as well as the AppImage and APK
  (`INSTALLER_REQUIRED_RELEASE_ASSETS`), and names whichever is missing
  ("Required installer is unpublished: ..."). Each one also has its own chip
  on `/status`. The Intel DMG and other assets are still downloaded under
  "Every artifact" but are not required by name.
- 2026-09-28: `/status` lists every check each system runs as chips under
  its title, with the detail on hover or focus (`checks` in `/api/status`;
  see [public-pages.md](public-pages.md), "Status"). A stored installer probe
  failure is now retried every half hour (`probe-installer-retry` at :03 and
  :33) instead of twice per six-hour window, so a fixed release or a deploy
  that fixes the probe clears the row within the hour, not at the next
  six-hourly sweep. A healthy row still costs one read per retry slot.
- 2026-09-28: Homepage downloads (and sometimes Installer) on `/status`
  went red about every third minute with "No reachable mirror served the
  homepage release metadata", and each time a Ping went out. The gateways
  were serving the data. The cause was the host's outbound HTTP/2 pool.
  Since 2026-09-27 it offered HTTP/2 to every origin, so all of a core's
  requests to `n2.next.forkmesh.com` shared one connection. The installer
  probe asks for each release blob (up to 16 MiB), checks the status and
  drops the body. httpcore gives each stream a 16 MiB window, the same size
  as the whole connection's window, and it never returns credit for data
  left unread when a stream is dropped. One dropped blob left the connection
  with no window. After that, any request on it got its headers but no body.
  The homepage probe's `/blobs` read, issued seconds later, timed out after
  20 s (`HTTPS_MIRROR_FETCH_TIMEOUT_SECONDS`) and the router gave up. Caddy
  showed those reads as `200`, `size 0`, "client disconnected" minutes
  later. Whichever core's pooled connections were starved at the time
  failed: n2 around 21:10 to 21:40 UTC, then n1 on every one of its minutes
  from 22:39. Fixes: HTTP/2 is now used only for the APNs hosts, which need
  it (`egress.HTTP2_ONLY_HOSTS`, `http2_mounts`), and every other origin
  uses HTTP/1.1, where dropping a body closes that one connection (see
  [../host/README.md](../host/README.md), Traps). The probes and the mirror
  proxy now cancel every body they do not read (`_discard_response_body`):
  release-blob checks, the flagship's `info/refs` read, oversized responses
  and failed upstream attempts. A probe-only `/status` row now also needs two
  failing probes in a row, normally from two different cores, before it
  turns red or alerts. Visitor evidence still turns a row red at once (see
  [public-pages.md](public-pages.md), "Status").
- 2026-09-28: `/status` has one monitor per pool runner (`runner:r1`, ...),
  green while the runner claimed or polled for jobs in the last three minutes
  (see [public-pages.md](public-pages.md), "Status").
- 2026-09-28: Every `/status` check now runs every minute. The flagship
  repository deep probe, the installer sweep and the homepage downloads probe
  were six-hourly, six-hourly and hourly (with `*-retry` phases for failures);
  the homepage, World and status-page synthetic probes cached a healthy result
  for ten minutes. Those budgets existed for the Cloudflare free plan. The
  cores take the cron in turn, so one-minute probes cost each core a probe
  every third minute, and a row that went red during an outage turns green
  within a minute of recovery instead of waiting up to six hours (as the
  flagship, installer and homepage-download rows did after the 2026-09-28
  core divergence). The `*-retry` phases are gone, since every run is a real
  probe; a new flagship failure still has to repeat on the next minute before
  it counts as downtime (`FLAGSHIP_FAILURE_CONFIRM_MS`). The installer sweep
  asks the gateway for each release blob, and the gateway hashes it before it
  answers, so it now costs one hash of every release artifact per minute
  across the cores. A busy minute can now need two cron invocations instead
  of one, because `CRON_BATCH_SUBREQUEST_CAP` still packs phases by measured
  cost.
- 2026-09-28: The status page's installer probe
  (`_installer_delivery_probe` in `status_monitoring.py`) accepts two release
  assets that share one digest. v1.2.5 publishes one universal macOS DMG as
  both `forkmesh-macos-arm64.dmg` and `forkmesh-macos-x86_64.dmg`, and the
  old rule that every digest be unique marked that signed release "incomplete
  or inconsistent". The probe now rejects a repeated asset name instead, and
  fetches each distinct digest once.
- 2026-09-28: a repository's Agents tab lists the owner's desktop agents
  when the desktop is running and manages them (transcript, prompt, stop,
  continue, diff, merge and clean up, delete) over the end-to-end encrypted
  control channel the phones use. New device kind `browser_control` with the
  single capability `desktop_control`. See
  [desktop-agents-web.md](desktop-agents-web.md). The same day its "On
  desktop" section was extended to organization repositories, where it
  connects only to the signed-in account's own desktops.
- 2026-09-27: Merge branches from the website. The branch-op queue gains a
  `merge` operation (a branch into the default branch or any other, pinned
  to both tips, optionally deleting the branch and its worktree afterwards),
  and the owning node now reports every outcome to
  `POST /api/repo/<o>/<r>/branches/ops/results` (table `branch_op_results`,
  migration 0219). The Branches tab and the compare page offer the merge with
  a conflict preview and show the node's report. See
  [branch-ops.md](branch-ops.md).
- 2026-09-27: `/api/calories/analyze` and `/api/calories/receipt` no longer
  answer 502 when package AI fails. The served stack has no AI provider
  (`AI_API_URL` unset), so every analyze call answered 502 and wrote
  `response status 502` to `error_log`. Now the answer is 503 `ai_unavailable`
  with a `reason` and a `retryable` flag: `retryAfterMs`/`Retry-After` for
  provider errors, 429 for rate limits, 400 for invalid images. It carries the
  expected-degraded marker. See [package-ai-failures.md](package-ai-failures.md).
- 2026-09-27: `POST /api/accounts/devices/check` answers whether a key is an
  enabled signing key of the caller's own account, so a desktop can let a
  phone on the same account control it without a pairing code
  ([auth-sessions](auth-sessions.md)).
- 2026-09-27: A retried pull request submission no longer opens a second
  pull request (issue #662, carried over from the 2026-09-01 branch
  `agent/issue-662-s2053-pr-creation-issue-through`). The desktop POSTs a
  signed pull request to `/pulls`; if the response was lost after the relay
  had accepted it, the retry reserved a new number and queued a second
  proposal. The signed event is now the retry key: `pulls_handler` stores
  an encrypted receipt in `pull_submission_receipts` (migration 0228), keyed
  by `blind_index("pull-submission:<author>:<sig>")`. The receipt outlives the
  `pull_inbox` row, so a replay after a mirror has drained the proposal still
  answers `{"ok": true, "number": n, "duplicate": true}` with the original
  number, and sends no notifications again. The receipt is written with
  `queued: false` before the inbox insert and set to `queued: true` after it;
  a replay that finds `queued: false` (the process died between the two
  writes) finishes the inbox write under the stored number. Concurrent
  retries race on `INSERT OR IGNORE` into the receipt table, and the loser
  reads back and uses the winner's number. The unique partial index
  `idx_pull_inbox_submission` on `pull_inbox(repo_bi, submission_bi)` makes
  the inbox insert idempotent too. A replay skips the inbox-full and
  per-author quota checks, since it adds no new proposal. The
  `submission_bi` column is added in `SCHEMA_PRE_CREATE_ALTER_STATEMENTS`
  because `SCHEMA_STATEMENTS` creates the index on it. The private surface
  audit (`app/src/private_surface_audit.py`) declares `pull_inbox.submission_bi`
  and inspects `pull_submission_receipts` as a repository payload table, and
  `tools/next_rekey.py` recomputes or pseudonymises both columns
  ([REKEY.md](../REKEY.md)).
- 2026-09-27: Immediate email no longer goes out for pull requests that are
  already merged or closed, or for a backlog that arrives late (issue #624).
  Agent runs open and merge pull requests in minutes, and the immediate rail
  sends at most `NOTIFICATION_INSTANT_MAX_ITEMS` per recipient per pass, so a
  burst queued up and was mailed after the pull requests had merged.
  `_notification_email_stale` now runs on each immediate ping, on both the
  immediate and the instant rails, just before it is mailed. It drops a ping
  older than `NOTIFICATION_IMMEDIATE_FRESH_MS` (an hour). For a
  `pull_submitted` or `pending_inbox` ping with source `pull` and a number, it
  reads `.forkmesh/pulls/<n>/pull.md` through the repository gateway (the read
  `microjobs` uses) and drops the ping if the `status` is anything other than
  `open`. A record that cannot be read (a private repository, or no mirror
  online) is mailed as before. Mentions and thread replies (`mention`,
  `subscribed`) are not checked. Each pass caches the reads per pull request
  and allows at most `NOTIFICATION_PULL_STATE_MAX_READS` of them per
  recipient. After that the scan stops and the rest waits for the next pass,
  so a slow gateway cannot stall the cron and nothing is mailed without being
  checked. A dropped ping still advances the watermark and stays unread in
  the inbox, so an existing backlog clears on the first pass after deploy
  instead of being mailed.
- 2026-09-27: Branch operations queued from the website. The
  Branches page's "Delete merged" and "Update all" sent one POST per branch
  into a queue capped at 64 per repository, so a repository with many merged
  agent branches failed partway with "Too many branch operations are already
  waiting". `POST /api/repo/<owner>/<repo>/branches/ops` now also takes
  `{"ops": [...]}`, which the two bulk actions use. Every entry is validated
  first (the default branch is refused, since deleting it or creating a second
  branch under its name is never what the caller means; an update with an
  empty `base` merges in the node's own default branch). Entries already
  waiting (same op and branch) are skipped, which also keeps a double-click
  idempotent, and the batch is refused whole with
  `{"error": "queue_full", "room": n}` when it does not fit, so a bulk action
  never leaves half its branches queued. The response carries `queuedCount`.
  The bound is now 1024 (`MAX_PENDING_BRANCH_OPS` and the
  `trg_branch_op_inbox_repo_bound` trigger, migration 0218). It exists only to
  bound a queue whose node never syncs; the desktop drains the whole queue on
  every sync and applies it off the GUI thread, so it has no limit of its own.
- 2026-09-27: The repository page's tab strip reads Code, Issues, Pull
  requests, Discussions, Insights, Projects, Actions (then Agents and
  Settings when shown). Issues took the slot Insights had after Code, and
  Pull requests and Discussions follow it, so the collaboration tabs sit
  together (`renderRepoDetail` in `dashboard/js/08-repo-detail-network.js`).
- 2026-09-27: Issue, pull request and discussion pings are emailed right away
  (issue #607). A digest-mode account used to wait for the hourly rollup and
  its three-minute read-in-app grace period for everything. A ping is
  "immediate" when its kind is `issue_opened`, `pull_submitted` or
  `issue_assigned`, or its source is `issue`, `pull` or `discussion`, which
  covers new items, comments, replies, mentions and inbox activity on all
  three (`notification_email_immediate`). For digest accounts the queue
  consumer, with the cron as backstop past the quarantine (or for the whole
  window when there is no queue), runs `_deliver_immediate_notifications`
  before the digest: one email per immediate ping, at most
  `NOTIFICATION_INSTANT_MAX_ITEMS` a pass out of a
  `NOTIFICATION_IMMEDIATE_SCAN`-row oldest-first scan. It keeps its own
  watermark, `last_immediate_ts`, because the digest watermark cannot advance
  past other pings that are still waiting for the rollup. The watermark stops
  just before the first ping that failed to send, so that ping is retried. When
  the account record has no `last_immediate_ts` yet, it starts from
  `last_digest_ts`, so the first pass after deploy does not email old pings.
  The digest leaves immediate pings out, so nothing is emailed twice. Instant
  delivery and the pause skip advance `last_immediate_ts` as well. Instant
  delivery skips immediate pings at or below that watermark, so switching from
  digest to instant does not re-send them. The settings copy on web, desktop
  and mobile now says so.
- 2026-09-27: `/@name` for an organization renders a GitHub-style
  organization page. `/api/accounts/<name>` answers 200 with `exists:false`
  for an unclaimed name, so `loadPublicProfile` (`dashboard/js/04-account.js`)
  only fell back to `/api/orgs/<name>` on a thrown fetch and showed
  `@forkmesh` as an empty user profile. It now also falls back on
  `exists:false`. `loadPublicOrganizationProfile` hides the user sidebar,
  about, referrals, contribution card and Network tab from JS, not CSS:
  `test_dashboard_landing_migration` finds `data-profile-*` names by first
  occurrence, and a `shell.html` selector would come first. A
  header shows the org logo or initial, display name, description, member
  count, address and creation month; the overview shows up to six popular
  repositories (ranked by mirror count, then commits) with People, Teams and
  Top languages (summed from each repository's `logoMetadata.languages`)
  beside them; and the Repositories tab uses the full width. The member list
  is not public (`/api/orgs/<name>/members` is forbidden), so People shows
  counts only.
- 2026-09-27: the dashboard pytest contracts pin current source again: the
  `actions` repository tab route, the compare-route `initialTab`, and task
  comments rendered by `taskActivityHtml` and posted from
  `data-task-reply-form`.
- 2026-09-27: error-log fixes. The package host marks a package Worker's
  5xx answer with error `missing_binding` or `ai_unavailable` (no AI provider
  on this host) `x-forkmesh-expected-degraded`, so it stays out of
  `error_log` even when the package does not set the header itself (group
  #43, `POST /api/calories/analyze`). The calorie counter's photo dialog
  says estimates are unavailable on this server when it gets a 503.
  `login.js` catches a failed login run (for example a cached pre-form page
  missing a field) and shows "Could not log in" instead of leaving an
  unhandled rejection and a disabled button (a likely source of group #130).
- 2026-09-27: Code challenges. Users race each other over a set number of
  days, scored from their public contribution days: a one-click 7-day
  default, two more presets, and custom rules (metric or custom weights,
  length, start, daily cap, size). Tables `code_challenges` and
  `code_challenge_members` (migration 0217), `/api/challenges`, and the
  `/dashboard/challenges` page. See [challenges.md](challenges.md).
- 2026-09-27: `/ledger`, `/network`, `/status` and `/mirror-payouts` moved to
  the website's page kit. The ledger's six-column table became compact rows
  (date, record with its people and linked issue, status, amount, a short
  receipt link) with a details panel for the full signature, wallets, source
  record and issue summary; the network page stacks host activity, user
  wallets (six rows, eligible and funded first, "Show all" for the rest) and
  top-five leaderboards under a live-relay card and a clone-route row; the
  status page's monitoring caveats moved below the systems list; and mirror
  rewards became a full page that says the program is not live yet. See
  [public-pages.md](public-pages.md).
- 2026-09-27: public App pages follow the website's redesign. The membership
  panel (`public/membership-prompt.js`, shared with the dashboard, World and
  the website) became Cal.com-style plan cards without green dots or tier
  artwork; `leaderboards.html` opens on the page kit from
  `www/public/styles.css`; `start.html` and `market-reviews.html` buttons use
  the textured button tokens; and every page title on `--display` now renders
  in ABC Favorit. Dashboard pages were left alone. See
  [docs/www/README.md](../www/README.md), "Recent changes".
- 2026-09-27: the `forkmesh-app` and `forkmesh-world` Workers were deleted
  ([CUTOVER.md](../CUTOVER.md), "Workers retired").
- 2026-09-27: fediverse digests publish again. A digest queue whose
  repository actor does not exist is deleted instead of blocking the queue
  (see Traps, "One stuck digest queue").
- 2026-09-27: a web agent session that failed on a provider login links to the
  Runners page to sign that runner in again
  ([../RUNNERS.md](../RUNNERS.md#web-sign-in)).
- 2026-09-26: ForkMesh Ecosystem. Table `ecosystem_projects` (migration
  0213), an Ecosystem section in the admin console (JSON import merged by
  slug, edit, publish, feature, delete, export) and the public
  `GET /api/ecosystem`, which lists published projects and never their
  private assessment. See [ecosystem.md](ecosystem.md).
- 2026-09-25: The repository page's "View on Mastodon" and fediverse handle
  links open the repo actor on the server of the Mastodon handle linked to
  the signed-in ForkMesh account (`fediverseFollowUrl` in
  `dashboard/js/04-account.js`). They used to always open mastodon.social,
  which asks anyone without an account there which server they are on before
  they can follow. Signed out, or with no handle linked, the link still opens
  mastodon.social.
- 2026-09-25: `/api/accounts/admin-usage-report` and
  `/api/accounts/admin-d1-usage-report` answer 200 with `ok:false`,
  `configured:false`, `error:"analytics_unconfigured"` when the Cloudflare
  analytics token is not set. The core nodes have no Cloudflare analytics,
  so that is the normal state, and the old 503 turned every desktop poll into
  a Caddy 5xx line and a red error in the node log feed. The body is
  unchanged apart from `configured`, and the desktop reads the body whatever
  the status, so it still shows its "no analytics token" notice and, for D1,
  storage and the read guard. `analytics_unavailable` (the token is set but
  Cloudflare did not answer) stays 502.
- 2026-09-24: `app.forkmesh.com` and `world.forkmesh.com` moved from the
  Cloudflare Workers to the core nodes ([CUTOVER.md](../CUTOVER.md)).
- 2026-09-24: Market ratings (be2ceb3), Turnstile on signup (c7804de), hosted
  repositories (a0061f7), server-rendered repository and pull/issue first
  views (74cc366, 3be0bd7), runner web sign-in (c9c2038), re-key of the
  imported data off the Cloudflare deployment's `DATA_KEY` (6d2cea5), Opus
  5.5 as the default agent model (348a751).
- 2026-09-24: a failed agent session shows its error once on the web and
  phone (`fe5a131cb`).
- 2026-09-24: phones are pinged when code is pushed; APNs payloads carry the
  origin (`5e79aa2e4`).
- 2026-09-23: CPython host (ba780c5), rqlite adapter (a47559a), clustered
  Durable Objects (cae6f88), runner tier (e92f6b3).
- 2026-09-23: PostHog replaced by the first-party `analytics` package
  (`12317aa01`).
- 2026-09-23: `/homev2`, `/homepage-update` and `/new-home` removed
  (`719089c51`).
- 2026-09-22: Web Requests stats came from the Worker log tail (now from the
  cores' Caddy access logs, see Operations); `api_metrics_minute` and
  `/api/metrics/summary` removed (migration 0190, `7d76ffd9c`).
- 2026-09-22: Polar payment integration removed (migration 0189); Patreon is
  the only payment rail (`dba3b4c7f`).
- 2026-09-22: the Worker moved to the Python 3.14 runtime (compatibility date
  2026-09-22), wrangler 4.136.2, workers-py 1.17 (`13b6fabc6`).
- 2026-09-19: clients stop display polling; transient merge 503s marked
  expected-degraded (`3be44c97d`).
- 2026-09-18: federation, SSH, sync, agent and MCP routes moved out of
  `entry.py` to fit the startup budget (`bd654b142`, `6288d1c20`).
- 2026-09-17: repository webhooks (migration 0188, `58a7cb08d`).
- 2026-09-17: ForkMesh Ads (migration 0187, `00723df7a`).
- 2026-09-16: web logins last five years across App, WWW and World
  (`1e6a107e5`).
- 2026-09-14: D1 read guard (migration 0186, `3e131fb44`).
- 2026-09-14: plan trials and usage meters (migration 0184, `3c8428089`).
- 2026-09-13: the cron tick runs inside the coordinator alarm (`3a11fb6f1`).

## Related docs

- [docs/host/README.md](../host/README.md),
  [docs/deploy/README.md](../deploy/README.md), [PLAN.md](../PLAN.md),
  [OPERATIONS.md](../OPERATIONS.md), [CUTOVER.md](../CUTOVER.md),
  [VAULT.md](../VAULT.md), [PERF_PAGES.md](../PERF_PAGES.md)
- [worker-footprint.md](worker-footprint.md),
  [cron-and-durable-objects.md](cron-and-durable-objects.md),
  [d1.md](d1.md), [payments-and-plans.md](payments-and-plans.md),
  [analytics.md](analytics.md), [auth-sessions.md](auth-sessions.md),
  [web-composer.md](web-composer.md),
  [web-chat-layout.md](web-chat-layout.md)
- `www/docs/operations/polling-elimination.md` (push-first client contract)
- `www/docs/operations/worker-split.md`,
  `www/docs/operations/cloudflare-free-tier-audit.md`,
  `www/docs/operations/d1-database-cleanup-plan.md` (Cloudflare era)
- `docs/operations/database-audit-2026-09-10.md`
- `www/docs/integrations.md`, `www/docs/design/activitypub-federation.md`
- `www/docs/operations/mobile-push-notifications.md`
- [docs/www](../www/README.md), [docs/world](../world/README.md)
