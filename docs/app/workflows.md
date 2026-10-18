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
| Desktop steps, the preinstalled Free disk space workflow, desktop run records | `app/src/workflow_desktop.py` |
| Script interpreter (Run script) | `app/src/workflow_script.py` |
| Integrations, declared as data | `app/src/workflow_integrations.py` |
| Routes, encrypted storage, credentials, triggers | `app/src/workflows_runtime.py` |
| n8n-compatible file format | `app/src/workflow_formats.py` |
| Examples | `app/src/workflow_templates.py` |
| `.forkmesh` repository files on the canvas | `app/src/workflow_files.py` |
| Page script | `app/public/dashboard/js/09-workflows.js` (rebuild with `app/tools/build_dashboard_assets.py`) |
| Desktop page | `marketplace/packages/workflows/content/desktop/` (`WorkflowsModel`, `WorkflowsFeature`, `WorkflowsDialogs.cpp`); test `forkmesh-workflows-tests` |
| Plug-in and nested packages | `marketplace/packages/workflows`, `workflows-pack-*`, `workflows-examples`, `workflows-repository-files` |
| Tables | `workflows`, `workflow_runs`, `workflow_credentials` (migration `0251_workflows.sql`) |
| Tests | `app/tests/test_workflows.py`, `test_workflow_actions.py`, `test_workflow_script.py`, `test_workflow_formats.py`, `test_workflow_files.py`, `test_workflow_desktop.py` |
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
| `GET /api/workflows/catalog` | Core nodes, integrations, operators, and `requests`: each integration's base URL, credential placement, headers and per-operation path, query and body templates (public) |
| `GET /api/workflows/desktop-script` | Source of `tools/free_disk_space.py` with each function's line range and the functions each desktop step runs (public); `script_unavailable` (404) when the server has no copy |
| `GET`/`POST /api/workflows` | List / create `{name, active, nodes, connections}` |
| `GET`/`POST /api/workflows/<id>` | Read / replace (`rotateHook: true` issues a new webhook token) |
| `POST /api/workflows/<id>/run` | Run now (`payload` overrides trigger test data); `runs_on_desktop` (409) for a workflow with desktop steps |
| `POST /api/workflows/<id>/record` | Store a run the browser drove on the desktop: `{startedAt, finishedAt, nodes, output: {freedBytes, freeBytes, totalBytes}}` |
| `POST /api/workflows/<id>/delete` | Delete with its runs |
| `GET /api/workflows/<id>/runs[/<runId>]` | Last 30 runs, each with its `output`; per-node report |
| `GET`/`POST /api/workflows/credentials`, `POST .../<id>/delete` | Masked list / create / delete |
| `GET /api/workflows/templates[/<key>]` | Examples |
| `POST /api/workflows/export`, `/convert`, `/import-url` | File format conversion |
| `POST /api/workflows/files/parse`, `/files/render`, `/files/new`; `GET /api/workflows/package-files` | Repository files |
| `GET`/`POST`/`PUT`/`PATCH /api/workflows/hook/<id>/<token>` | Webhook trigger, no session; item `{method, query, body}` |

## Step details and scripts

Selecting a step opens its panel with a **What this step is** section above
the settings (`wfAboutHtml`), so every step says what it is and what it does
before you edit it:

- **Kind** (`wfNature`): Trigger, Script (Run script), Part of a script (a
  desktop step, an Action's Run step, or a block of a `.sh`/`.bash` shortcut),
  Integration call, Web request, Runs another workflow, Built-in step, Note,
  or in the Repository view Part of a prompt, Part of a skill, Agent setting
  and File header. Scripts and parts of scripts also carry a **Script** or
  **Part of script** tag on the canvas, next to the step's type.
- **Facts**: the step type id, where it runs, and the steps it receives from
  and sends to. Each of those is a link that selects that step, so you can
  walk the workflow from the panel. Run workflow steps link to the workflow
  they run, Run shortcut steps to the shortcut.
- **Integrations** show the request: method and URL with the step's settings
  filled in, the chosen credential and how it is sent (header, query, Basic
  sign-in or the whole URL; secret values never appear), and under
  **What it sends** the headers, query and body the engine builds, placing
  fields the way `build_app_request` does. The templates come from the
  catalog's `requests` map, which stays out of `engine.catalog()` so the phone
  engines' catalog parity is unchanged.
- **Settings** lists every field's value in words, then the step's saved JSON.

The **script area** under the facts:

- **Desktop steps** name the file (`tools/free_disk_space.py`), the
  `--step` they run, the exact command the desktop runs, the dry run that
  deletes nothing, the desktop function that starts it, and the script's
  functions this step uses (`workflow_desktop.FUNCTIONS`, in `script` on each
  catalog node). **View script** or a function chip opens a wide viewer with
  each of those functions, numbered by their lines in the file, and the whole
  file with this step's code marked. The source comes from
  `/api/workflows/desktop-script`, which parses the server's copy with `ast`;
  the desktop runs the copy in its own checkout.
- **Run script** shows its size, how often it runs and its last log; the
  viewer shows the code with line numbers and the selected run's log.
- **Repository files**: a script block, an Action Run step, a prompt section
  or a skill section shows its place (block N of M) and its text; **View in
  the whole file** renders the file through `/api/workflows/files/render` and
  marks the lines that step becomes.

## Run output

Every run has an `output` `{text, metrics: [{label, value}]}`, stored in the
run report and returned by `/runs`, so the recent runs list shows a little
window on each row. A server run's output is `workflows.run_output`: the
item count of its last steps (the steps nothing connects onward from, or the
Stop step) and a short preview of the first item, its `output`, `text`,
`message`, `result` or `status` field, or else its first three plain fields.
A failed run's output is its error. A desktop run's output is the bytes freed
and the free space after (`workflow_desktop.disk_output`).

### Send a failure to an agent

A failed run (the one just run, or one picked in the recent runs list) shows
**Send to agent** in three places: the progress rail's headline, the
Selected execution panel and the failed step's Last run section. It puts a
report in the agent prompt ([web-composer.md](web-composer.md)) through
`sendTextToPrompt`, and the user picks the repository and agent and sends it;
on a page without a prompt it copies the report instead (`wfSendFailure`).
The report (`wfFailurePrompt`) names the workflow and its page link, the run
and its error, every step with its type, status, time and error, and then for
the failed step:

- a desktop step: the `tools/free_disk_space.py --step <step>` command the
  desktop ran, the dry run that repeats it, and the docs to read;
- any other step: its settings as JSON (at most 1500 characters), with values
  whose key, or whose pair's `name`, looks like a token, secret, password, API
  key, auth header, cookie or signature replaced by `[hidden]`;
- its last 40 log lines, when it logged any.

The report stops at 7500 characters, under the prompt's 8000. A desktop step
that fails sends only the last line it printed (the desktop's
`cleanup_failed` message), so that is the error the report carries.

## Desktop steps

The **Desktop** group (`workflow_desktop.NODES`, `runsOn: "desktop"`) holds
steps that only the user's own desktop can run: the six parts of
`tools/free_disk_space.py` ([../tools/free-disk-space.md](../tools/free-disk-space.md)),
one step type each so each has its own icon.

- **Who runs them.** Not the server and not the phones: the server engine and
  both phone engines fail such a step with its `refusal` text, and
  `POST /<id>/run` answers `runs_on_desktop`. **Run workflow** in the browser
  (`wfRunOnDesktop`) connects to the first linked desktop through
  `desktopControlHub()` ([desktop-agents-web.md](desktop-agents-web.md)), then
  for each step in graph order sends `diag.prepare` and `diag.run` with action
  `freeDiskSpace` and waits for the desktop's answer (up to 20 minutes a step).
  Then it posts the per-step report to `/<id>/record`, which checks every node
  id, status and duration (`workflow_desktop.clean_report`) and stores it as a
  `desktop` run.
- **Rules.** A workflow with a desktop step must start from Manual run and may
  hold only desktop steps besides No operation and notes (`desktop_needs_manual`,
  `desktop_steps_only`): schedules and webhooks have no browser to drive them,
  and server steps cannot run in the same pass.
- **Preinstalled.** Listing workflows (`_list`) adds **Free disk space** to an
  account that has never had it: Manual run and the six steps in a row, saved
  like a workflow the user built. Its id is derived from the account
  (`seed_id`). Deleting it keeps an ownerless, empty row under that id so it
  is not added again, and needs no new table.
- **The progress rail.** Under the canvas of a workflow with desktop steps,
  `wfRenderRail` draws a bar whose full width is the longest recorded run. A
  thin line marks how long each other run took, with the time under it. While a
  run goes, the bar fills in the accent colour and the running step's icon
  slides along it with the elapsed time, redrawn every 250 ms. Each finished
  step's icon stays where that step ended, ringed in green or red. A selected
  past run places its step icons at their cumulative times.

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

On the desktop the plugin is a native page too ([On the desktop](#on-the-desktop)).
On iPhone and Android the plugin is a native screen, and a phone can run a
workflow itself: the package's `content/mobile/workflow-engine.js` runs in
JavaScriptCore on iPhone, Android runs a Kotlin port, and both replay cases
produced by this engine. After changing `workflows.py`,
`workflow_integrations.py` or the run limits, run
`python3 marketplace/tools/workflows_mobile_bundle.py` and keep the phone
engines passing; see [native/WORKFLOWS.md](../native/WORKFLOWS.md).

## On the desktop

Clicking **Workflows** on the desktop rail opens a native page
(`builtin:workflows`, API prefix `/api/workflows`, proof
`forkmesh-workflows-request-v1`) instead of the browser. It calls the same
routes as the web canvas through the host's `request`, which sends the
account session as a Bearer token, so the desktop must be signed in; a 401 or
403 `plugin_not_installed` shows as a sentence in the status line
(`errorText` maps every server error code).

- **Dashboard.** Stats: workflows (and their step total), active (scheduled,
  webhook, manual), runs (today), success rate (succeeded and failed),
  average duration, last run with the last failed workflow, next scheduled
  run. Your workflows lists name, trigger (a schedule shows its interval),
  steps, on/off, last run and result, runs kept (failed count), next run and
  last change, filtered by the box or the window's top search. Edit, Run,
  Turn on/off, Duplicate (saved off, as "Copy of ...") and Delete act on the
  selected row; a double click opens it. Recent runs merges the 30 kept runs
  of every workflow that has run (`GET /<id>/runs`, at most four at a time,
  only for workflows with a `lastRunAt`), newest first, filtered by workflow
  and result; selecting one shows each step's result, time, item count and
  error, first item or last log line, with the log in the tooltip.
- **Editor.** The graph is an ordered list of steps like the phones'
  (`Draft::ordered`, Kahn order). The form is built from the catalog fields:
  select, boolean, text/json/code (fixed-width for json and code),
  string/value/number. Integrations add Operation (switching keeps the
  values of fields the new operation shares) and Credential with **Manage...**.
  Every step but the trigger has **Runs after**: its inputs, each with
  Disconnect, and Connect for any step and branch that would not make a
  loop (`Draft::canConnect`), so Merge steps can take several inputs. A new
  step goes one column (288) right of its parent and one row (120) down per
  sibling; removing a step joins its parents to its children; changing the
  trigger keeps its id and connections; a trigger added to a graph without
  one feeds every unconnected step. Values keep their JSON type: a number or
  object stays one when the edit still parses. Webhook workflows show their
  URL with Copy and New URL (`rotateHook`). Save posts the whole graph; Run
  workflow saves first; Download writes the `/export` n8n file; leaving with
  unsaved changes asks to Save, Discard or Cancel.
- **New workflow.** Blank (Manual trigger), any example from
  `/templates`, **Import file...** (`/convert`, files up to 2 MB) or
  **Import from link...** (`/import-url`). All open unsaved and off, and
  conversion warnings are listed.
- **Credentials.** Lists the masked credentials, adds one for any
  integration with credential fields (secret fields are hidden while typed)
  and deletes with a confirmation.
- **Desktop steps run here.** A workflow with desktop steps runs on this
  desktop after a confirmation naming each step: the page finds a repository
  with `tools/free_disk_space.py` through the host's `repositories` (the
  manifest's `repository.read` permission; `forkmesh/forkmesh` first), then
  runs `python3 -u tools/free_disk_space.py --execute --step <step> --repo
  <checkout> --json` per step in graph order, the same command the desktop's
  local-control runner uses (`freeDiskSpaceArguments`), with the same
  20-minute limit and the same skip rules as `wfRunOnDesktop`. The process
  is named `forkmeshFreeDiskSpaceProcess` and parented to the page's window,
  so while the page sits in the main window it and a browser-driven run
  refuse to overlap. The report goes to
  `/<id>/record` like a browser-driven run.
- **Search and badge.** The top search box finds workflows by name; the rail
  badge counts workflows whose last run failed.

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

The `actions-schedule` cron phase (every minute) is a different mechanism:
it queues `.forkmesh/*.yml` workflows with an `on.schedule` cron on an
instance's own runner ([../server/instance-actions.md](../server/instance-actions.md)).
