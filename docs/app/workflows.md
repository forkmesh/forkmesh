# Workflows (visual automation builder)

Part of [the App docs](README.md). A workflow is an n8n-style automation: a
trigger followed by steps connected on a canvas. Each step is a core node
(HTTP request, Set fields, If, Filter, Merge, Limit, Split out, No operation)
or one of 100 integrations (Slack, GitHub, Notion, Stripe, Claude, OpenAI,
Google Sheets, HubSpot and the rest). Items flow along the connections; every
step runs once per item it receives.

## Code map

| Piece | Where |
| --- | --- |
| Engine: node types, graph validation, expressions, request building, execution | `app/src/workflows.py` (pure Python, no `js` imports) |
| The 100 integrations, declared as data | `app/src/workflow_integrations.py` |
| Routes, encrypted storage, credentials, webhook and schedule triggers | `app/src/workflows_runtime.py` |
| Tables `workflows`, `workflow_runs`, `workflow_credentials` | `app/migrations/0251_workflows.sql`, `app/src/schema.py` |
| Route dispatch | `WORKFLOWS_API_RE` in `app/src/urls.py`, `app/src/app_runtime.py`; `_workflows` lazy module in `app/src/entry.py` |
| Cron phase `workflow-schedules` (every 5 minutes) | `_cron_due_phases` / `_run_cron_phase` in `app/src/entry.py`, `app/src/cron_activity.py` |
| Standard file format (n8n-compatible JSON) | `app/src/workflow_formats.py` |
| The 10 examples | `app/src/workflow_templates.py` |
| Repository files: actions, shortcuts, skills, special agents | `app/src/workflow_files.py` |
| The plug-in and its nested packages | `marketplace/packages/workflows` (manifest, page markup `content/web/view.html`, `page.js`, desktop link `content/desktop/WorkflowsLink.cpp`, shipped repository files), `marketplace/packages/workflows-pack-*`, `workflows-examples`, `workflows-repository-files` |
| Page script | `app/public/dashboard/js/09-workflows.js` (a per-page chunk reached through `builtinPage("workflows")`), `workflowsView` in `01-state.js` |
| Phone Hub entry | `HubItem.workflows` in `native/ios/ForkMeshCore/Sources/FMApi/Navigation/RouteCatalog.swift` and `HomeShell.openWorkflows`; `HubItem.Action.Workflows` in Android `RouteCatalog.kt` and `NavigationScreens.kt` |
| Shipped-file staging | `content_files` in `app/tools/build_app_plugins.py` |
| Tests | `app/tests/test_workflows.py`, `test_workflow_formats.py`, `test_workflow_files.py` |

## Architecture

**Graph.** A workflow is `{name, nodes, connections}`. A node has an `id`
(chosen by the browser), a `type` (`core.*` or `app.<integration>`), a unique
`name`, a canvas position, `params`, an optional `credential` id and the flags
`disabled` and `continueOnFail`. A connection is `{from, output, to}`; If has
the outputs `true` and `false`, every other node has `main`. `clean_workflow`
keeps only known parameter keys, allows at most one trigger, 60 nodes and 120
connections, refuses connections into a trigger, and refuses cycles (Kahn's
algorithm), so execution order is always a topological order.

**Execution.** `workflows.Execution.run` walks the topological order. The
trigger emits one item. A node collects the items of every incoming
connection (so Merge, or any node with several inputs, concatenates them),
is skipped when nothing arrived, and otherwise runs. A step error stops the
run and marks the remaining nodes skipped, unless the node has
`continueOnFail`, in which case it emits `{"error": "..."}` and the run goes
on. Each node keeps at most 100 items; the stored run report keeps the first
5 items of each output, each cut at 4,000 characters.

**Expressions.** A parameter can contain `{{ ... }}`. The expression is a
path, never code: a root (`$json`, `$node`, `$trigger`, `$now`, `$today`,
`$item`, `$workflow`) followed by `.name`, `[0]` or `["key"]` steps. Anything
else is an error. A parameter that is a single expression keeps the value's
type (a number stays a number); otherwise the parts are joined as text.
`$node["Name"].json` is the item at the same index in that step's output, or
its first item. Renaming a step in the editor rewrites `$node["Old"]`
references.

**Integrations are data.** Each entry in `INTEGRATIONS` has a base URL, an
auth scheme and operations. An operation has a method, a path with `{field}`
placeholders, its fields and optionally a body template. Field values fill
the path (URL-encoded; `slashes` fields keep `/`), then go to the query
(GET) or the JSON body (other methods), or into the template; a template
string that is exactly `{field}` takes the typed value, and an empty optional
field drops its key. Auth schemes: bearer token, a custom header with an
optional prefix, query parameter, HTTP Basic (templates over credential
fields, so Mailgun's `api:<key>` and Zendesk's `<email>/token` fit), values
merged into the body (Pushover, PagerDuty, Amplitude), and webhook URLs.
Credential fields marked `url` hold a server or webhook URL and are placed
into the base verbatim; every other credential value is URL-encoded. A header
whose credential value is empty is left out (ntfy's optional token). The
`output` key on chat operations adds a top-level `text` with the reply
(`anthropic`, `openai`, `gemini`, `cohere`), because Claude's first content
block can be a thinking block.

The Anthropic step uses `claude-opus-5-5`, `anthropic-version: 2023-06-01`,
and the server-side refusal fallback (`anthropic-beta:
server-side-fallback-2026-07-01` with `"fallbacks": "default"`). Its default
`max_tokens` is 4,096 rather than larger because a step must answer inside the
30-second request timeout without streaming.

To add an integration, add one `APP(...)` entry; `test_every_integration_is_well_formed`
checks its placeholders and `test_every_operation_builds_a_safe_request_with_sample_values`
builds every operation. Keep the count at what the page and docs say (the test
pins 100).

**A plug-in.** Workflows is the `workflows` marketplace package. Like Tasks,
its host code stays in the App: the manifest's `worker` and `desktop`
entrypoints are `builtin:workflows`, the `dashboard.page` contribution
supplies the route, title, navigation and markup, and `page.js` calls
`builtinPage("workflows")`, which runs the core chunk. `install.sync` is
`account`, so installing or removing it on one device applies to every device.
When it is removed for an account, `/api/workflows` answers 403
`plugin_not_installed` (the catalog and examples stay public), webhooks answer
409 and scheduled runs are skipped; saved workflows and credentials are kept,
so installing it again brings them back. The page shows an Install button.
Since 1.1.0 (2026-10-13) it is off by default: `install.default` is false and
`installedBefore` is 2026-10-14, so accounts and projects created before then
keep it and newer ones install it from the Market. Its Home tile, rail button
and links follow the package on every app, and each install picks the apps it
shows in ([core-plugins.md](../marketplace/core-plugins.md#2026-10-13-where-each-plugin-shows-up)).

**Nested packages.** Each nested package depends on `workflows` and carries
data only, the way the Status checks do:

| Package | Socket | What turning it off does |
| --- | --- | --- |
| `workflows-pack-<category>` (12) | `workflows.integrations` `{category, integrations}` | Hides that category's integrations from Add step. Existing steps keep running. |
| `workflows-examples` | `workflows.templates` `{examples}` | Hides Examples. |
| `workflows-repository-files` | `workflows.repository` `{kinds}` | Hides the Repository view; `/api/workflows/files/*` and `package-files` answer 403. |

`workflows.packs()` derives the pack ids from the integration categories, and
`test_workflows_is_a_marketplace_plugin_with_nested_packs` checks every pack
manifest lists exactly the integrations of its category. The overview panel
lists the packs with switches that call `/api/plugins/@account/<id>/install`
or `uninstall`.

**Standard file format.** Download writes the n8n workflow shape: `nodes` with
`position: [x, y]` and `parameters`, and `connections` keyed by step name,
with If's outputs as indexes 0 (true) and 1 (false). Core steps are written as
their n8n node types (`n8n-nodes-base.if`, `httpRequest`, `set`,
`scheduleTrigger` ...) with n8n parameter shapes and `={{ ... }}` expressions,
so the logic opens in n8n; integration steps are written as `forkmesh.app.<key>`.
Each node also carries `forkmeshType` and `forkmeshParams`, which n8n ignores
and which make a ForkMesh round trip exact. Upload and Import from a URL accept
any n8n workflow file, n8n template pages (`n8n.io/workflows/<id>` is read from
`api.n8n.io`), GitHub blob and Gist links. Unknown steps become No operation
steps with the same name so the graph keeps its shape, and the import lists
everything to check (a second trigger, extra If conditions, credentials, loops
that were cut). Imports open unsaved.

**Repository files.** The Repository view opens a repository's `.forkmesh`
files on the same canvas:

- Actions (`.forkmesh/*.yml`): a trigger step (events, changed paths, branches,
  runs-on, needs, checkout, fetch depth, environment), then job and run steps
  in connection order. `workflow_files` carries a port of the Go YAML subset
  reader (`server/actions_workflow.go`) and writes only the subset the Go and
  desktop parsers read. Comments are kept: comments above the file, a job or a
  step become that step's Comments field and are written back above it. Every
  action file in this repository that the Go parser accepts reads back
  identically in Go after a rewrite (checked with the Go parser during the
  change); `.forkmesh/clean-machine-vm.yml` is rejected by both parsers.
- Shortcuts (`.forkmesh/shortcuts/*`) and skills (`.forkmesh/skills/*.skill`):
  a header step (`# name:`, `# description:`, and for shortcuts `# agent:` and
  `# model:`), then one block per paragraph, joined by a blank line.
- Special agents (`.forkmesh/teammates/<id>.json`): the agent in the middle;
  schedule, events and log monitors on the left; playbook, skill files, native
  workflow and relay channel on the right. Only steps connected to the agent
  are saved. Validation matches `TeammateStore` (modes, events, permissions,
  `HH:mm`, days 1 to 7, 16 monitors, `id` equals the file name). Fields the
  editor does not show (activity, learning journal, run bookkeeping) are kept,
  and the file is written in Qt's `QJsonDocument::Indented` layout, so the
  desktop's own saves produce no diff.

Saving renders the file on the server (`/api/workflows/files/render`), shows
any warning (a step that is not connected, a setting ForkMesh Actions ignores),
and commits it through `POST /api/repo/<owner>/<repo>/file-edit` to the
default branch with the blob oid it was read at, so a concurrent change is a
409 instead of a lost edit. New files use `create: true`.

**Files a plug-in ships.** The `content.action`, `content.shortcut`,
`content.skill` and `content.teammate` sockets (see the
[marketplace docs](../marketplace/README.md#protocols-and-interfaces)) let any
package ship `.forkmesh` files. The Repository view lists them under From
plug-ins; choosing one opens it as a new file to review and save. Workflows
ships the Weaver special agent, the Build a workflow and Check ForkMesh files
shortcuts, the Workflow design skill and the Check ForkMesh files action.

**Phones and desktop.** The page is built for touch: below 1024 px the list
is a drawer and step settings a bottom sheet, pinch zooms, a long press on
empty canvas adds a step, and ports grow on coarse pointers. The phone Market
shows the package with Open on the web, and the iOS and Android Hub's App group
has a Workflows entry that opens `/dashboard/workflows` on the signed-in
project's server. The desktop registers `builtin:workflows` as a browser link
(`WorkflowsLink.cpp`), so the Market and rail open the page.

## Protocols and interfaces

Every answer is `no-store` JSON except the catalog. Writes need a user session
(node sessions are refused), and cookie sessions pass the origin check in
`_account_session_record`. Errors are `{"ok": false, "error": "<code>"}`;
`WF_ERROR_TEXT` in `09-workflows.js` maps each code to a sentence.

| Route | What it does |
| --- | --- |
| `GET /api/workflows/catalog` | Core nodes, the 100 integrations with their operations and credential fields, categories and operators. Public, `max-age=300` |
| `GET /api/workflows` | The caller's workflows (summaries) |
| `POST /api/workflows` | Create: `{name, active, nodes, connections}`. 201 |
| `GET /api/workflows/<id>` | One workflow with its nodes, connections and `hookPath` |
| `POST /api/workflows/<id>` | Replace the definition; `rotateHook: true` issues a new webhook token |
| `POST /api/workflows/<id>/run` | Run now from the manual trigger; `payload` overrides the trigger's test data |
| `POST /api/workflows/<id>/delete` | Delete the workflow and its runs |
| `GET /api/workflows/<id>/runs`, `GET /api/workflows/<id>/runs/<runId>` | Last 30 executions; one execution's per-node report |
| `GET`/`POST /api/workflows/credentials` | List (names and masked hints only) or create `{integration, name, values}` |
| `POST /api/workflows/credentials/<id>/delete` | Delete a credential |
| `GET /api/workflows/templates`, `GET /api/workflows/templates/<key>` | The examples (public, `max-age=300`) |
| `POST /api/workflows/export` | `{document, filename}` for a definition, in the standard format |
| `POST /api/workflows/convert` | `{document}` (text or object) to `{workflow, warnings}` |
| `POST /api/workflows/import-url` | `{url}` to `{workflow, warnings, source}`; the server downloads the public https link |
| `POST /api/workflows/files/parse`, `/files/render`, `/files/new` | Repository file to canvas document and back; a new file's starting document |
| `GET /api/workflows/package-files` | Repository files that installed plug-ins ship |
| `GET`/`POST`/`PUT`/`PATCH /api/workflows/hook/<id>/<token>` | Webhook trigger, no session. The item is `{method, query, body}`; a body that is not JSON arrives as `{"text": ...}` |

## Configuration

Limits live at the top of `workflows.py` and `workflows_runtime.py`: 50
workflows and 50 credentials per account, 120 runs per account per hour
(manual, webhook and schedule together), 30 runs kept per workflow, 30 s per
request, 256 KB read per response, 64 KB webhook bodies. Requests per run and
the run's time budget depend on the trigger: manual 25 requests / 45 s,
webhook 15 / 25 s, schedule 6 / 40 s.

## Operations

The `workflow-schedules` cron phase runs every five minutes (minutes ending in
3 or 8) and starts at most three due workflows (oldest `next_run_at` first), so
a schedule fires up to five minutes after it is due and the platform runs at
most 36 scheduled workflows an hour. It is not every minute because the cron
coordinator packs a minute's phases into invocations by their measured
high-water cost: a phase that can make about 35 subrequests would cost an extra
Durable Object invocation in every minute it is due. Raising the capacity means
moving scheduled runs to a queue consumer rather than adding cron slots. It moves `next_run_at` forward
before running each one, so a failing workflow cannot be retried in a loop.
Three workflows of six requests plus their D1 writes fit inside the phase's
45-subrequest budget (`CRON_PHASE_D1_QUERY_LIMIT`). A schedule is 5 to 10,080
minutes; saving keeps the existing `next_run_at` when the interval did not
change.

## Security

- Workflow definitions, run reports and credentials are encrypted with
  `encrypt_row`. Only the owner's `owner_bi`, status and timestamps are plain
  columns. Credential values are never returned: the list shows a masked hint
  (`••••` and the last four characters of a secret).
- A credential is used only when its owner and integration match the step, so
  a workflow cannot borrow another account's credential by id.
- Steps call only `https://` URLs with a public-looking host: no IP literals
  outside global space, no `localhost`, `.local`, `.internal` or
  `forkmesh.com` names, no user info. The host's egress guard
  (`app/host/forkmesh_host/egress.py`) still checks every connection and
  redirect hop at connect time, so DNS answers pointing inward are refused
  there.
- The webhook token is 256 random bits, compared with
  `hmac.compare_digest`, and the URL answers only while the workflow is active
  and its trigger is a webhook.
- Expressions are paths, not code. There is no Code node: nothing a user
  types is executed on the core nodes.

## Traps

- The page script is the core chunk `09-workflows.js`: after editing it, run
  `app/tools/build_dashboard_assets.py`. The page itself is staged from the
  package by `build_app_plugins.stage_web` into
  `dashboard-plugins/workflows/default/` at deploy.
- The canvas styles are a scoped `<style>` block in the package's
  `content/web/view.html` (as on the Notes page) rather than Tailwind
  utilities, so the prebuilt `tailwind.css` did not need regenerating.
- `trigger` is an SQL keyword, so the runs column is `trigger_kind`.
- `/dashboard/workflows` must not be in `dashboard_shell.PAGES`: the plug-in
  staging refuses a route a core page already owns. It stays in
  `CHUNK_PAGES` and `LAZY_CHUNKS` so the plug-in page preloads the chunk, and
  `"workflows"` is in both `builtinPage` allowlists in `10-boot.js`.
- Locally the checked-in `plugin_registry.py` is empty, so every Workflows
  package counts as installed and `package-files` is empty; the deploy build
  generates the real registry in `app/src_build`.
- The YAML writer quotes anything that is not a plain token and writes lists
  in flow style only for events, needs and runs-on, the forms the Go and
  desktop parsers share; changed paths are always a quoted block list.
- Node coordinates are clamped to ±20,000; the edge layer is an SVG offset by
  that much so connections are hit-testable anywhere on the canvas.

## Recent changes

- 2026-10-09: Workflows became the `workflows` plug-in with 14 nested packages,
  works on phones (touch layout, Hub entry, Market link) and from the desktop
  rail, edits repository actions, shortcuts, skills and special agents on the
  canvas, downloads and uploads n8n-compatible files and imports from links,
  ships 10 examples, lets connections be moved by their handles or by
  dragging a step's input, and shows a summary of each step's settings on the
  canvas.

- 2026-10-09: first version: the canvas editor, 11 core nodes, 100
  integrations, credentials, manual, webhook and schedule triggers, and
  execution history (migration 0251).
