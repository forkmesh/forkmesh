# Workflows (visual automation builder)

Part of [the App docs](README.md). A workflow is an n8n-style automation: one
trigger (manual, webhook or schedule) followed by steps on a canvas at
`/dashboard/workflows`. Each step is a core node (HTTP request, Set fields, If,
Filter, Merge, Limit, Split out, No operation), one of the Shortcuts-style
scripting, text, number, list and date steps including Run script
([workflow-steps.md](workflow-steps.md), [workflow-scripts.md](workflow-scripts.md)),
or one of 100 integrations (Slack, GitHub, Notion, Stripe, Claude, OpenAI,
...). Items flow along connections; each step runs once per item it receives.

## Code map

| Piece | Where |
| --- | --- |
| Engine (graph, expressions, execution) | `app/src/workflows.py` |
| Scripting, text, number, list and date steps | `app/src/workflow_actions.py` |
| Script interpreter (Run script) | `app/src/workflow_script.py` |
| Integrations, declared as data | `app/src/workflow_integrations.py` |
| Routes, encrypted storage, credentials, triggers | `app/src/workflows_runtime.py` |
| n8n-compatible file format | `app/src/workflow_formats.py` |
| Examples | `app/src/workflow_templates.py` |
| `.forkmesh` repository files on the canvas | `app/src/workflow_files.py` |
| Page script | `app/public/dashboard/js/09-workflows.js` (rebuild with `app/tools/build_dashboard_assets.py`) |
| Plug-in and nested packages | `marketplace/packages/workflows`, `workflows-pack-*`, `workflows-examples`, `workflows-repository-files` |
| Tables | `workflows`, `workflow_runs`, `workflow_credentials` (migration `0251_workflows.sql`) |
| Tests | `app/tests/test_workflows.py`, `test_workflow_actions.py`, `test_workflow_script.py`, `test_workflow_formats.py`, `test_workflow_files.py` |
| Phone engine and bundle | `marketplace/packages/workflows/content/mobile/workflow-engine.js`, `marketplace/tools/workflows_mobile_bundle.py` |

## Building a workflow

- **Graph**: `{name, nodes, connections}`. A node has `id`, `type` (`core.*` or
  `app.<integration>`), unique `name`, position, `params`, optional
  `credential`, and flags `disabled` / `continueOnFail`. A connection is
  `{from, output, to}`; If has outputs `true` and `false`, Choose by value
  `case1`-`case3` and `otherwise`, others `main`.
  Limits: one trigger, 60 nodes, 120 connections, no cycles, nothing into the
  trigger.
- **Execution** runs in topological order. A node concatenates items from all
  inputs, is skipped if none arrived, and a step error stops the run unless
  `continueOnFail` (then it emits `{"error": ...}`). 100 items per node. A
  Stop workflow step ends the run as a success; the report then names it in
  `stopped`. Run script steps add their `console.log` lines to the step's
  report as `logs`.
- **Expressions** `{{ ... }}` are paths, never code: a root (`$json`, `$node`,
  `$trigger`, `$now`, `$today`, `$item`, `$vars`, `$workflow`) then `.name`, `[0]` or
  `["key"]`. A parameter that is one expression keeps its type.
  `$node["Name"].json` is that step's item at the same index.
- **Credentials** are created per integration (`/api/workflows/credentials`)
  and referenced by id; a step uses one only if owner and integration match.
- **Integrations** are `APP(...)` entries (base URL, auth scheme, operations
  with `{field}` path placeholders and optional body template). Chat operations
  add a top-level `text` with the reply. Adding one is one entry; the tests
  check every operation and pin the count.
- **Repository files**: the Repository view edits `.forkmesh/*.yml` actions,
  `.forkmesh/shortcuts/*`, `.forkmesh/skills/*.skill` and
  `.forkmesh/teammates/<id>.json` special agents on the canvas. Saving renders
  on the server (`/api/workflows/files/render`), reports warnings, and commits
  through `POST /api/repo/<owner>/<repo>/file-edit` with the read blob oid (a
  concurrent change is a 409). Packages can ship such files through the
  `content.*` sockets ([marketplace docs](../marketplace/README.md#protocols-and-interfaces)).
  A shortcut script's body is split into blocks on blank lines. A block
  whose first non-empty line is `# step: <name>` is a named step and the
  canvas shows that name (at most 80 characters) instead of "Commands N". A
  block with a line `bash .forkmesh/shortcuts/<file> ...` becomes a
  `shortcut.run` node ("Run shortcut", blue): it reads "Runs <file>" and its
  Open link loads that shortcut on the canvas; the first such line wins. The
  desktop and phone apps parse the same rules. Only the block text is saved,
  so the file round-trips byte for byte (a missing final newline is kept in
  `meta.finalNewline`), and editing the link away just saves the text.
- **Import/export**: Download writes n8n workflow JSON (plus `forkmeshType` /
  `forkmeshParams` for exact round trips). Upload and Import from URL accept
  n8n files, n8n template pages, GitHub blobs and Gists; unknown steps become
  No operation, and imports open unsaved with a list of warnings.

## API

Writes need a user session. Errors are `{"ok": false, "error": "<code>"}`.

| Route | Purpose |
| --- | --- |
| `GET /api/workflows/catalog` | Core nodes, integrations, operators (public) |
| `GET`/`POST /api/workflows` | List / create `{name, active, nodes, connections}` |
| `GET`/`POST /api/workflows/<id>` | Read / replace (`rotateHook: true` issues a new webhook token) |
| `POST /api/workflows/<id>/run` | Run now (`payload` overrides trigger test data) |
| `POST /api/workflows/<id>/delete` | Delete with its runs |
| `GET /api/workflows/<id>/runs[/<runId>]` | Last 30 runs; per-node report |
| `GET`/`POST /api/workflows/credentials`, `POST .../<id>/delete` | Masked list / create / delete |
| `GET /api/workflows/templates[/<key>]` | Examples |
| `POST /api/workflows/export`, `/convert`, `/import-url` | File format conversion |
| `POST /api/workflows/files/parse`, `/files/render`, `/files/new`; `GET /api/workflows/package-files` | Repository files |
| `GET`/`POST`/`PUT`/`PATCH /api/workflows/hook/<id>/<token>` | Webhook trigger, no session; item `{method, query, body}` |

## Limits

50 workflows and 50 credentials per account, 120 runs per account per hour,
30 runs kept, 30 s per request, 256 KB per response, 64 KB webhook body.
Per run: manual 25 requests / 45 s, webhook 15 / 25 s, schedule 6 / 40 s.
Schedules are 5 to 10,080 minutes; the `workflow-schedules` cron phase runs
every five minutes, starts at most three due workflows, and advances
`next_run_at` before running so failures never loop. A Wait step pauses at
most 60 s and never past the run's deadline; Run workflow shares the calling
run's requests and deadline and nests two levels deep.

## Plug-in

Workflows is the `workflows` marketplace package (`builtin:workflows`,
`install.sync: account`; [core-plugins.md](../marketplace/core-plugins.md)).
Uninstalled, `/api/workflows` answers 403 `plugin_not_installed`, webhooks 409
and schedules skip; data is kept. Nested packages add integration categories,
examples and the Repository view.

On iPhone and Android the plugin is a native screen, and a phone can run a
workflow itself: the package's `content/mobile/workflow-engine.js` runs in
JavaScriptCore on iPhone, Android runs a Kotlin port, and both replay cases
produced by this engine. After changing `workflows.py`,
`workflow_integrations.py` or the run limits, run
`python3 marketplace/tools/workflows_mobile_bundle.py` and keep the phone
engines passing; see [native/WORKFLOWS.md](../native/WORKFLOWS.md).

## Security

- Definitions, run reports and credentials are encrypted (`encrypt_row`);
  credential values are never returned.
- Steps call only public `https://` hosts (no IP literals outside global space,
  no internal names, no user info); the host egress guard re-checks every
  connection and redirect.
- Webhook tokens are 256 random bits compared with `hmac.compare_digest`, valid
  only while the workflow is active with a webhook trigger.
- Run script code is interpreted by `workflow_script.py`, a sandboxed
  JavaScript subset with no access to the host, files or the network, a step
  limit, a call-depth limit and size caps ([workflow-scripts.md](workflow-scripts.md)).
  Nothing a user types reaches `eval`, `exec` or a JavaScript engine.

## Web styling

`view.html` styles the page with its own `wf-*` classes but takes every colour
and size from the dashboard theme tokens. `.wf-btn` is the 32px control,
`.wf-btn-primary` is the green create colour (Run workflow, Add first step,
Import), the active toggle is blue, and run, node and list status dots use the
success, destructive and muted tokens. The File menu is a popover with 30px
rows and muted icons, `.wf-notice` tints its top bar with the success,
destructive or attention colour from `data-tone`, the dialogs use the shell
dialog radius and shadow, and group headings are sentence case instead of
capitals.

## Traps

`/dashboard/workflows` must not be in `dashboard_shell.PAGES` (it stays in
`CHUNK_PAGES` / `LAZY_CHUNKS` and the `builtinPage` allowlists in `10-boot.js`).
The runs column is `trigger_kind`. Locally `plugin_registry.py` is empty, so
every package counts as installed.
