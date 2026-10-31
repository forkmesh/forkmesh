# App (`app/`)

The App is the ForkMesh control plane: the web dashboard, every `/api/*`
route, account sessions, Git Smart HTTP for hosted and proxied repositories,
ActivityPub, the chat and World room objects, the per-minute cron, and the
database. It serves `app.forkmesh.com` and, with `world/`,
`world.forkmesh.com`. Both names resolve by DNS round robin straight to three
self-hosted core nodes; each core runs Caddy, this Python code on CPython
(`app/src`, staged to `app/src_build`, under the host in
`app/host/forkmesh_host/`), rqlite and the Go daemon, and a DNS health loop
(`tools/next_dns.py`) drops a failed core. With two cores down the survivor
serves reads and refuses writes ([PLAN.md](../operations/served-stack.md);
[architecture](../architecture.md)). Agent jobs run on the runner
tier ([RUNNERS.md](../operations/runners.md)). The host is documented in
[docs/host/README.md](../host/README.md); this page covers the App code.

The code keeps the API shape it was written for on Cloudflare Workers: a
Worker entrypoint, bindings named after D1, KV, Queues and Durable Objects,
and `app/wrangler.toml`. On the cores the host provides all of them (rqlite
for D1, clustered room objects for Durable Objects). The static forkmesh.com
site (the `forkmesh-www` Worker) proxies its dynamic traffic here (see
[docs/www](../www/README.md) and [docs/world](../world/README.md)). The
desktop, the Go daemon, the runner, the mobile apps and browsers are the
App's clients. Developer docs index: [../README.md](../README.md); public,
user-facing descriptions live in `docs/` ([index](../README.md)).

## Code map

| Path | Responsibility |
| --- | --- |
| `app/src/app_runtime.py` | The `Default` WorkerEntrypoint: `fetch`, `scheduled`, `queue`; stateless fast paths and the handoff to `entry.py` |
| `app/src/entry.py` | The router, most route handlers, the Durable Object classes (`ForkMeshRoom`, `ForkMeshNodes`, `ForkMeshNoteRoom`, `ForkMeshCronRunner`, `ForkMeshCronWatchdog`, `ForkMeshDiscordGate`, `ForkMeshOrgOffice`), crypto helpers, `ensure_schema` |
| `app/src/*.py` (other modules) | Domain modules loaded on demand through `_LazyModule` |
| `app/src/schema.py` | Runtime DDL and the schema fingerprint stored in `schema_meta` |
| `world/src/*.py` | World, Office and organization task routes and the `ForkMeshWorld` / `ForkMeshOfficeRoom` Durable Objects; staged into the App |
| `marketplace/packages/*/content/worker/*.py` | Server code of marketplace packages, staged next to the App modules |
| `app/migrations/NNNN_*.sql`, `app/migrations/down/` | Numbered roll-forward migrations and their down scripts (World migrations too) |
| `app/wrangler.toml` | Not deployed. Source of `[vars]`, binding lists, queue consumer settings and `run_worker_first`, read by `app/host/forkmesh_host/env.py` and `deploy/core/caddy_routes.py` |
| `app/host/` | The CPython host: ASGI front, bindings, rqlite D1 adapter, clustered Durable Objects, vault, request guard, IDS records |
| `app/public/` | Static pages (login, signup, dashboard shells, chat, notes, status) |
| `app/public/dashboard/js/*.js` | Dashboard fragments, concatenated into `app/public/dashboard.js` by `app/tools/build_dashboard_assets.py`. Git merges fragments as plain text, so a merge can produce a bundle that does not parse; `test_composed_bundle_and_chunks_parse` runs `node --check` on the core bundle and every chunk |
| `app/tools/build_worker_python.py` | Stages `app/src`, `world/src` and package modules into `app/src_build/` with comments stripped |
| `world/tools/build_worker_footprint.py` | Source footprint report and static budgets |
| `app/tests/`, `app/tests/host/`, `app/browser_tests/` | Python tests, host tests, Playwright specs |

## Topic pages

| Area | Pages |
| --- | --- |
| Platform | [cron-and-durable-objects](cron-and-durable-objects.md), [d1](d1.md), [worker-footprint](worker-footprint.md), [auth-sessions](auth-sessions.md), [login-page](login-page.md), [update-notice](update-notice.md), [docs-book](docs-book.md), [world-boot-latency](world-boot-latency.md), [dashboard-colors](dashboard-colors.md), [dashboard-components](dashboard-components.md), [seasonal-theme](seasonal-theme.md) |
| Repositories | [repo-views](repo-views.md), [ai-review](ai-review.md), [pull-merge-box](pull-merge-box.md), [pull-quick-replies](pull-quick-replies.md), [repo-features](repo-features.md), [repo-settings-access](repo-settings-access.md), [repository-traffic](repository-traffic.md), [repository-activity](repository-activity.md), [repository-file-edits](repository-file-edits.md), [ai-disclosure](ai-disclosure.md), [pull-review-prompt](pull-review-prompt.md), [directory-about](directory-about.md), [branch-ops](branch-ops.md), [remote-forks](remote-forks.md), [provider-mirrors](provider-mirrors.md), [archive-sync](archive-sync.md), [issue-scale-limits](issue-scale-limits.md), [public-pages](public-pages.md), [feeds-and-lite](feeds-and-lite.md), [mirror-quota](mirror-quota.md), [translations](translations.md), [forum-reach](forum-reach.md), [branch-protection](branch-protection.md), [diff-viewer](diff-viewer.md), [issue-views](issue-views.md), [issue-assignment-deadline](issue-assignment-deadline.md), [issue-assignment-limit](issue-assignment-limit.md), [comment-edits](comment-edits.md), [comment-reactions](comment-reactions.md), [comment-gifs](comment-gifs.md), [comment-tips](comment-tips.md), [bot-avatars](bot-avatars.md) |
| Chat | [chat-protocol-v3](chat-protocol-v3.md), [chat-protocol-v4](chat-protocol-v4.md), [chat-relay-hardening](chat-relay-hardening.md), [chat-relay-server](chat-relay-server.md), [web-chat-relay-hardening](web-chat-relay-hardening.md), [chat-sealed-sender](chat-sealed-sender.md), [chat-private-rooms](chat-private-rooms.md), [chat-direct-calls](chat-direct-calls.md), [chat-group-calls](chat-group-calls.md), [face-stream](face-stream.md), [fmm-browser](fmm-browser.md), [chat-push-previews](chat-push-previews.md), [chat-read-sync](chat-read-sync.md), [chat-unread-reminders](chat-unread-reminders.md), [chat-everyone-channels](chat-everyone-channels.md), [chat-genie-welcome](chat-genie-welcome.md), [chat-formatting](chat-formatting.md), [chat-stickers](chat-stickers.md), [web-chat-layout](web-chat-layout.md), [chat-member-stats](chat-member-stats.md), [web-composer](web-composer.md) |
| Notifications | [notification-email](notification-email.md), [new-issue-pings](new-issue-pings.md), [content-first-pings](content-first-pings.md), [thread-notifications](thread-notifications.md) |
| Notes | [notes-sharing](notes-sharing.md), [notes-convert](notes-convert.md), [notes-activity](notes-activity.md) |
| Tasks | [task-progress](task-progress.md), [issue-to-task](issue-to-task.md), [task-markdown](task-markdown.md), [task-trash](task-trash.md), [task-pings](task-pings.md), [task-email](task-email.md), [task-board-filters](task-board-filters.md) |
| Agents and desktop | [agents-page](agents-page.md), [desktop-agents-web](desktop-agents-web.md), [desktop-commands](desktop-commands.md), [settings-agents-and-runners](settings-agents-and-runners.md), [agent-notes](agent-notes.md), [activity-today](activity-today.md), [workflows](workflows.md), [workflow-steps](workflow-steps.md), [workflow-scripts](workflow-scripts.md) |
| Accounts and organizations | [account-data](account-data.md), [member-roster](member-roster.md), [username-casing](username-casing.md), [organization-bot-scopes](organization-bot-scopes.md), [payments-and-plans](payments-and-plans.md), [badges](badges.md), [discussion-answers](discussion-answers.md), [web-discussions](web-discussions.md), [web-desktop-parity](web-desktop-parity.md), [web-releases](web-releases.md), [web-repo-access](web-repo-access.md), [web-projects](web-projects.md), [challenges](challenges.md), [feedback-bounties-links](feedback-bounties-links.md), [early-ios-testers](early-ios-testers.md), [accounts-and-moderation](accounts-and-moderation.md) |
| Operations and growth | [server-update](server-update.md), [analytics](analytics.md), [not-found-tracking](not-found-tracking.md), [probe-auto-ban](probe-auto-ban.md), [signup-failure-pings](signup-failure-pings.md), [password-reset-challenge](password-reset-challenge.md), [marketing-email](marketing-email.md), [onboarding-email](onboarding-email.md), [ecosystem](ecosystem.md), [ecosystem-client-work](ecosystem-client-work.md), [package-ai-failures](package-ai-failures.md), [privacy-mode](privacy-mode.md), [affiliates](affiliates.md), [instance-federation](instance-federation.md), [instance-legal-pages](instance-legal-pages.md) |

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

**Durable Objects** (room objects emulated by the host). `ForkMeshRoom` (chat rooms and relay sockets),
`ForkMeshNodes` (per-owner event sockets), `ForkMeshNoteRoom` (live notes),
`ForkMeshWorld` and `ForkMeshOfficeRoom` (World presence and Office meetings),
`ForkMeshCronRunner` (cron coordinator and ingress shards),
`ForkMeshCronWatchdog`, `ForkMeshDiscordGate`, `ForkMeshOrgOffice`
(organization offices, `app/src/org_office_room.py`). Each object is placed on one
live node by rendezvous hash; storage and alarms live in rqlite. See
[cron-and-durable-objects.md](cron-and-durable-objects.md).

**Storage.** `DB` is the database of record and keeps D1's SQL dialect. Most
row payloads are AES-GCM blobs under `DATA_KEY` with HMAC blind indexes for
lookups. Git bytes are never stored in the database; they live on desktop
nodes and in the cores' Git stores ([HOSTED_REPOS.md](../operations/core-hosted-repositories.md)).
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
over the unseal socket ([VAULT.md](../operations/vault.md)). How data is encrypted:
[encryption](../encryption.md); threats considered:
[threat model](../protocol/threat-model.md).

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
  nothing. The versioned integration contract is
  [integrations](../integrations.md).
- Public snapshots: anonymous GET routes served from validated
  `public_api_snapshots` rows (`app/src/public_snapshots.py`). The catalog
  schema is an exact allow-list: a new public field in `/api/repositories`
  (for example a mirror's `mirrorOf`, `upstreamCommit`, `lastSyncedAt` or a
  fork's `forkedFrom`) must be added to `CATALOG_REPOSITORY_KEYS`, or every
  catalog snapshot is rejected and the route falls back to a live read (or
  returns 503 while an expired privacy tombstone is still in place).
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
  [docs/operations/fleet-tooling.md](../operations/fleet-tooling.md) and
  [OPERATIONS.md](../operations/README.md). A node that fails its gate reverts and
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
  (`tools/next_rekey.py`, [REKEY.md](../operations/rekey.md)).
- The host applies a Host allowlist, streamed body caps, an SSRF egress guard
  and edge-client verification before the App sees a request
  (`request_guard.py`, `egress.py`).
- Signup and password-reset forms require Turnstile (`app/src/turnstile.py`)
  or its backup check.
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
  `shared_read`. With no `d1_read_guard` row the switch is `auto`
  (conserve once today's rows-read sample reaches 60%);
  `tools/rqlite_migrate.py` stamps `off` unless `--d1-read-guard` says
  otherwise.

## Related docs

- [docs/host/README.md](../host/README.md),
  [docs/operations/fleet-tooling.md](../operations/fleet-tooling.md), [PLAN.md](../operations/served-stack.md),
  [OPERATIONS.md](../operations/README.md), [VAULT.md](../operations/vault.md)
- [polling elimination](../operations/polling-elimination.md)
  (push-first client contract), [integrations](../integrations.md),
  [ActivityPub federation](../design/activitypub-federation.md)
- Pillars: [architecture](../architecture.md),
  [encryption](../encryption.md),
  [mirroring](../mirroring.md),
  [self-hosting](../self-hosting.md),
  [censorship resilience](../censorship-resilience.md)
