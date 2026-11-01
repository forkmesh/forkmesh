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
serves reads and refuses writes ([served stack](../operations/served-stack.md);
[architecture](../architecture.md)). Agent jobs run on the runner
tier ([runners](../operations/runners.md)). The host is documented in
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

One page per topic. Each links back here.

### Platform and data

| Page | Covers |
| --- | --- |
| [cron-and-durable-objects](cron-and-durable-objects.md) | Room objects, the per-minute cron and ingress shards |
| [d1](d1.md) | The `DB` database: rqlite, schema, read budget, traps |
| [worker-footprint](worker-footprint.md) | App source footprint report and World module budgets |
| [auth-sessions](auth-sessions.md) | Session tokens, cookies, login state, admin 2FA, email confirmation |
| [login-page](login-page.md) | The `/login` page: styling rules, demo login, redirects |
| [signup-challenge](signup-challenge.md) | Signup and password reset Turnstile challenge, backup check, signup failure pings |
| [update-notice](update-notice.md) | Dashboard notice when a new version is deployed |
| [docs-book](docs-book.md) | Developer docs page `/dashboard/docs` and in-place editing |
| [member-roster](member-roster.md) | Public member roster and avatar endpoints, privacy rules, boot latency on the cores |
| [analytics](analytics.md) | First-party product analytics, consent, recordings and error reports |
| [not-found-tracking](not-found-tracking.md) | 404 tracking, the admin 404s list and automatic probe bans |
| [server-update](server-update.md) | Admin console: Update ForkMesh, repository addresses, legal pages |
| [privacy-mode](privacy-mode.md) | Privacy mode for self-hosted instances |
| [instance-legal-pages](instance-legal-pages.md) | Terms, Privacy and Security pages on self-hosted instances |
| [instance-federation](instance-federation.md) | Self-hosted instances joining the directory and World |
| [public-pages](public-pages.md) | Public ledger, network, status and mirror rewards pages |
| [early-ios-testers](early-ios-testers.md) | Early iOS tester flag and build notifications |

### Dashboard look and parts

| Page | Covers |
| --- | --- |
| [dashboard-colors](dashboard-colors.md) | Dashboard colour roles and theme.json |
| [dashboard-components](dashboard-components.md) | Shared `fm-*` dashboard components |
| [web-composer](web-composer.md) | The dashboard prompt composer |
| [web-desktop-parity](web-desktop-parity.md) | What works on the web and what still needs the desktop |
| [seasonal-theme](seasonal-theme.md) | The platform-wide Halloween theme |

### Repositories and pull requests

| Page | Covers |
| --- | --- |
| [repo-views](repo-views.md) | How repository pages are drawn: views, tabs, title row, About rail |
| [repo-settings](repo-settings.md) | Who administers a repository, feature switches, collaborators and visibility |
| [repo-media](repo-media.md) | Videos in issue and pull request Markdown |
| [repository-activity](repository-activity.md) | Insights tab: Pulse, leaderboard, milestones, clone traffic |
| [repository-file-edits](repository-file-edits.md) | Editing, adding, renaming and deleting files on the web |
| [recent-push-banner](recent-push-banner.md) | The recent pushes banner and Compare &amp; pull request |
| [branch-ops](branch-ops.md) | Branch merge, update and delete from the website |
| [branch-protection](branch-protection.md) | Protected default branch and the Actions switch |
| [diff-viewer](diff-viewer.md) | Web diff viewer for pulls, compare and commit pages |
| [directory-about](directory-about.md) | Folder About boxes: API, README fallback, web page |
| [pull-merge-box](pull-merge-box.md) | Pull request merge box, Update branch, quick review replies and conflict pings |
| [pull-review-prompt](pull-review-prompt.md) | The Diff and files box: summary link and review prompt |
| [pull-revisions](pull-revisions.md) | Updating a patch-backed pull request with revisions |
| [remote-forks](remote-forks.md) | Forks on other instances and cross-instance pull requests |
| [provider-mirrors](provider-mirrors.md) | Mirroring a repository to GitHub or GitLab |
| [archive-sync](archive-sync.md) | Scheduled backup of a repository to the Internet Archive |
| [ai-review](ai-review.md) | Automated first-pass AI review of pull requests |
| [ai-disclosure](ai-disclosure.md) | AI disclosure on pull requests and the issue AI prompt |
| [security-advisories](security-advisories.md) | Private vulnerability reports and repository security advisories |
| [web-releases](web-releases.md) | Releases and tags on the web |
| [web-projects](web-projects.md) | Projects on the web |
| [web-discussions](web-discussions.md) | Discussions on the web |
| [discussion-answers](discussion-answers.md) | Marking a discussion reply as the answer |

### Issues, tasks, notes and notifications

| Page | Covers |
| --- | --- |
| [issue-assignment](issue-assignment.md) | Assign button, one-open-issue rule and assignment deadlines |
| [issue-scale-limits](issue-scale-limits.md) | Issue and forum scale limits |
| [issue-to-task](issue-to-task.md) | Moving an issue to an organization task |
| [issue-views](issue-views.md) | Issue view counts |
| [comment-edits](comment-edits.md) | Editing and deleting your own issue and pull request comments |
| [comment-reactions](comment-reactions.md) | Comment reactions, GIFs and stickers in comments, Solana Pay tips |
| [tasks](tasks.md) | Organization tasks: Tasks page, Markdown text, progress and work items, trash, pings, email |
| [notes](notes.md) | Web Notes: sharing and access link, activity log, note to task or issue, body search, platform differences |
| [notifications](notifications.md) | Thread notifications, new issue pings, notification email send claims, collaboration author display |
| [content-first-pings](content-first-pings.md) | Pings that lead with the actor and the content |
| [feeds-and-lite](feeds-and-lite.md) | Feeds, the lite view and the forum home page |
| [forum-reach](forum-reach.md) | Static forum exports, publishing and onion service |
| [accounts-and-moderation](accounts-and-moderation.md) | Forum accounts without email, random names, moderators, blocks, hidden posts, new-account limits |
| [translations](translations.md) | Translating posts |

### Chat and calls

| Page | Covers |
| --- | --- |
| [chat-protocol-v3](chat-protocol-v3.md) | Chat protocol v3 specification |
| [chat-protocol-v4](chat-protocol-v4.md) | Chat protocol v4: per-message ratchet for direct messages |
| [chat-relay-hardening](chat-relay-hardening.md) | Relay hardening rules R1-R7 for clients and the server side |
| [chat-sealed-sender](chat-sealed-sender.md) | Sealed sender for direct messages (proposed) |
| [chat-private-rooms](chat-private-rooms.md) | Private rooms and direct messages: access, delivery, media, DM surfaces |
| [chat-everyone-channels](chat-everyone-channels.md) | #general, #random and #welcome server channels and the genie spider welcome |
| [chat-unread](chat-unread.md) | Read marks across devices and unread reminder emails |
| [chat-formatting](chat-formatting.md) | Chat message formatting syntax and renderers |
| [chat-stickers](chat-stickers.md) | Chat stickers on messages, per platform |
| [chat-member-stats](chat-member-stats.md) | The activity line under each person in the chat people pane |
| [chat-push-previews](chat-push-previews.md) | Message text in chat push notifications |
| [web-chat](web-chat.md) | The `/chat` page layout and styling, and browser relay hardening |
| [chat-direct-calls](chat-direct-calls.md) | One-to-one encrypted calls: signaling, resilience, rings, quality widget, phones |
| [chat-group-calls](chat-group-calls.md) | Group audio and video calls in private channels |
| [call-filters](call-filters.md) | Face, background, outfit and voice filters in calls, native ports, Face Changer |
| [face-stream](face-stream.md) | Low-bandwidth face stream for calls |
| [fmm-browser](fmm-browser.md) | FM Media call engine in the browser |

### Agents, apps and workflows

| Page | Covers |
| --- | --- |
| [agents-page](agents-page.md) | The Agents page and desktop agents on a repository's Agents tab |
| [agent-notes](agent-notes.md) | AI guestbook and anonymous agent notes (GET-only endpoints) |
| [desktop-commands](desktop-commands.md) | Signed phone-to-desktop commands through the relay |
| [settings-agents-and-runners](settings-agents-and-runners.md) | Settings → Agents &amp; runners |
| [activity-today](activity-today.md) | Today's activity heat grid shared across a member's devices |
| [workflows](workflows.md) | Workflows: the visual automation builder |
| [workflow-steps](workflow-steps.md) | Shortcuts-style workflow steps and the Run script interpreter |
| [oauth-apps](oauth-apps.md) | ForkMesh Apps: OAuth 2.1 installs, scopes, Web and Events APIs, directory |
| [organization-bot-scopes](organization-bot-scopes.md) | Organization bot token scopes and retired scopes |
| [package-ai-failures](package-ai-failures.md) | How package AI failures surface (Calorie Counter) |

### People and accounts

| Page | Covers |
| --- | --- |
| [account-data](account-data.md) | Phone Settings &gt; Data: saved settings, the data inventory and the export |
| [profile-links](profile-links.md) | Profile links for names and mentions on every surface, profile page avatars |
| [username-autocomplete](username-autocomplete.md) | The shared username autocomplete picker |
| [username-casing](username-casing.md) | Username capitalization rules |
| [bot-avatars](bot-avatars.md) | Built-in avatars for bots and system accounts |
| [badges](badges.md) | Achievement badges, levels, record badges and medallions |
| [challenges](challenges.md) | Code challenges between users |
| [payments-and-plans](payments-and-plans.md) | Patreon memberships, role grants, plan usage |

### Growth and community

| Page | Covers |
| --- | --- |
| [attribution](attribution.md) | UTM capture, acquisition attribution and reporting |
| [affiliates](affiliates.md) | Affiliate program: claims, CRM link, payment and tax forms |
| [marketing-email](marketing-email.md) | Opt-in marketing email (consent, unsubscribe, campaigns) and the drafted onboarding emails |
| [plugin-race](plugin-race.md) | The Plugin Race hackathon |
| [feedback-bounties-links](feedback-bounties-links.md) | Feedback, bounties and link rewards in the dashboard |
| [ecosystem](ecosystem.md) | Curated ecosystem project directory |
| [ecosystem-client-work](ecosystem-client-work.md) | Analysis behind the Ecosystem's work and client management category |

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
nodes and in the cores' Git stores ([core-hosted repositories](../operations/core-hosted-repositories.md)).
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
over the unseal socket ([vault](../operations/vault.md)). How data is encrypted:
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
  [operations](../operations/README.md). A node that fails its gate reverts and
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
  (`tools/next_rekey.py`, [rekey](../operations/rekey.md)).
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
  [docs/operations/fleet-tooling.md](../operations/fleet-tooling.md), [served stack](../operations/served-stack.md),
  [operations](../operations/README.md), [vault](../operations/vault.md)
- [polling elimination](../operations/polling-elimination.md)
  (push-first client contract), [integrations](../integrations.md),
  [ActivityPub federation](../design/activitypub-federation.md)
- Pillars: [architecture](../architecture.md),
  [encryption](../encryption.md),
  [mirroring](../mirroring.md),
  [self-hosting](../self-hosting.md),
  [censorship resilience](../censorship-resilience.md)
