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
| Desktop page | `marketplace/packages/workflows/content/desktop/` (`WorkflowsModel`, `WorkflowsCards` for the list, thumbnail and compact cards, `WorkflowsRunFlow` for the animated run steps and the new-run sparkle, `WorkflowsFeature`, `WorkflowsDialogs.cpp`); test `forkmesh-workflows-tests` |
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
  `.forkmesh/shortcuts/*`, `.forkmesh/skills/*.skill`,
  `.forkmesh/workflows/<id>.json` desktop workflows (see
  [Desktop workflows and special agents](#desktop-workflows-and-special-agents))
  and legacy `.forkmesh/teammates/<id>.json` special agents on the canvas. Saving renders
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
account session as a Bearer token or, when the desktop holds no session for
the server, signs the request with the account key (`node`, `ts`, `sig` over
the proof, method, path and the body's SHA-256). The server accepts either:
`_session_account` falls back to `_plugin_scope_signed_session` with
`REQUEST_PROOF`, as Notes does. Before 2026-10-18 it took only the session, so
a desktop without one got a 401 on the list while the public catalog loaded,
and the phones showed workflows the desktop did not. A 401 or 403
`plugin_not_installed` shows as a sentence in the status line (`errorText`
maps every server error code).

- **Dashboard.** Stat tiles: workflows (and their step total), active
  (scheduled, webhook, manual), runs (today), success rate (succeeded and
  failed), average duration, last run with the last failed workflow, next
  scheduled run; they count every workflow in the list, the desktop's too.
  Below them is **one list** of every workflow: special agents, the
  repositories' desktop workflows and the account's workflows on ForkMesh
  (see [One list](#one-list-on-the-desktop)). Recent runs merges the 30 kept runs
  of every workflow that has run (`GET /<id>/runs`, at most four at a time,
  only for workflows with a `lastRunAt`), newest first, filtered by workflow
  and result; selecting one shows the run's steps as an animated chain
  (`RunFlow`, `workflowsRunFlow`), under it the run's output
  (`workflowsRunOutput`): each step's output, log lines and error under its
  name, and under that a table of each step's result, time, item count and
  error, first item or last log line, with the log in the tooltip.
- **Run goes to Recent runs.** Run, Run with an assignment, a rail pin and the
  editor's Run workflow all follow the new run (`followRun`): the run filters
  reset (the workflow filter stays when it already names that workflow), the
  runs pane grows to at least 40% of the page and the new run is
  selected. A desktop run is matched by the `runId` the `run` action returns
  (special agents return it too), else by the first run of that workflow
  started within two minutes of the click. An account run has no id until
  `POST /<id>/run` answers, so a "Running" placeholder row (`m_pendingRun`)
  stands in for it until then. Every Run makes that placeholder at once
  (`startPendingRun`), for an account run, a repository workflow and a run
  of desktop steps alike, with the workflow's steps as Waiting when the page
  knows them (`pendingSteps`), so the run appears and its steps show before
  anything answers. It stays until the real run is in the list: an account
  run swaps it for the run `POST /<id>/run` returns (`landPendingRun`), a
  desktop run once a list holds the `runId` the `run` action returned, and a
  pending one with a `runId` goes after two minutes. Desktop steps fill the
  placeholder as each one starts and ends (`updateDesktopPending`).
- **Run animation.** The user asked (2026-10-27) for a new run to sparkle into
  Recent runs and for the right side to show the flow at once, each step
  animated, with the log below. When the followed run's row first appears,
  `RunSparkle::burst` puts a short-lived overlay on the table's viewport: a
  white flash, a sweep across the row and stars, 1.4 s, then it deletes
  itself. It finds the row by run id each frame and keeps the last place when
  the placeholder is replaced. `RunFlow` draws one circle per step joined by
  lines: Waiting is numbered, Running spins with a pulsing halo and a dot
  running into it along the line, Succeeded, Failed, Skipped and Stopped get
  a check, cross, dash or square with a pop and a ring, and a line fills in
  green after a step succeeds. While a run is live with no running step (an
  account run, which answers only when finished) a light runs along the whole
  chain; with no steps yet it says Starting. When several steps change in
  one update they turn one after another (160 ms apart), so an account run's
  result plays through its steps. A new run's steps fade in one by one;
  steps of the same run, or of the run that replaced the placeholder
  (`m_flowAliasFrom`/`m_flowAliasTo`), keep their state and only animate what
  changed. The timer runs at 30 fps only while something moves or the run is
  live and the page is visible. The chain scrolls sideways past about 104 px
  a step and keeps the running step in view; each circle's tooltip has the
  step's result, time and detail.
  waiting, the page asks the host for it (`get`, which returns the active run)
  every second and redraws the steps and output, keeping the output scrolled
  to the end unless you scrolled up. The runner fills a running step's
  `output` and `logs` from each poll (an agent's latest reply, a shortcut's or
  process's output so far) in memory only; the run file is still written
  when a step starts or ends.
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
- **Editor layout.** The user asked (2026-10-27) for a more compact editor
  that shows more. It is three columns: the steps, the selected step's form,
  and the runs (the runs table above the selected run's steps). Before, the
  runs sat under the form at full width, the steps list took 40% of the
  width, and most of the page was empty. Each step is a card painted by
  `StepDelegate` (`WorkflowsCards`): a numbered badge on a line joining the
  steps, coloured by kind (trigger accent, special agent purple, desktop
  step teal, else grey, dashed when turned off), its name, its type and
  summary, and what it runs after. "Not connected" shows in the attention
  colour. A dot on the right gives that step's status in the latest run.
  The item text keeps all three lines for accessibility. Under the name bar,
  chips (`workflowsFact`, `renderEditorFacts`) show the state (off,
  running, last run succeeded or failed), the trigger, the next and last
  run, runs with the share that succeeded, the step count, and where the
  workflow lives: its repository and file, or ForkMesh. Hovering the file
  chip shows the desktop note that used to be a line of prose. Checkbox
  groups with more than four options use four columns, so Days and Run on
  events each take two rows. The runs table reads Started, Result,
  Duration, Trigger, Output. A desktop run's step table names its fourth
  column Session rather than Items. A special agent's icon slot shows its
  portrait. The page subtitle is hidden while the editor is open.
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

### One list on the desktop

The user asked (2026-10-24) for the special agents to join the account's
workflows in one list, for every workflow to get the special agent features,
and for a list, a thumbnail and a compact grid view with little controls and
the latest output under each one. Before, the page had two tables: **On this
desktop** (repository workflows and special agents) and **Your workflows**
(the account's).

- **Entries.** `unifiedWorkflows` (`WorkflowsModel.cpp`) turns both into one
  entry shape: `key` (`local:<repo>|<id>` or `account:<id>`), `source`
  (`local`, `account`), `kind` (`agent`, `desktop`, `account`) and its label
  (Special agent, Desktop workflow, On ForkMesh), `place`, trigger text,
  `enabled`, schedule, steps, last run and status, runs and failures, next
  run, rail pin, and the latest output: a desktop run's caption or summary
  (`lastSummary`) and output file, or the newest account run's
  `runOutputText`. `statsWorkflows` maps entries to the stats input.
- **Toolbar.** Chips filter by kind: All, Special agents, On this desktop, On
  ForkMesh, Pinned, Needs attention (failed or broken) (`workflowMatches`, which
  also matches the filter box against name, description, trigger,
  repository, kind and output). Sort by name, last run, next run or kind
  (`sortWorkflows`). Three view buttons pick List, Thumbnails or Compact grid;
  the view and sort are kept in `workflows/viewMode` and `workflows/sort`.
- **Cards** (`WorkflowsCards.*`). Thumbnails: a painted header tinted by kind
  (violet special agents, blue desktop, teal ForkMesh) with the icon or
  portrait, a small drawing of its steps, a state pill and the rail it is
  pinned to; then name, kind chip, place and trigger, the latest output (two
  lines) and run times. Compact grid: icon, name, kind and last run, a state
  dot and one line of output. List: one row per workflow with its output,
  trigger, state and next or last run under column headings. Cards flow into
  as many columns as fit (`CardGrid`; 270 px thumbnails, 210 px compact).
- **Controls under every card.** Run (Stop while it runs), Edit, Pin to rail,
  Start or Pause schedule (desktop workflows with a schedule), Last output, an
  on/off switch and More: Run now, Run with an assignment, Edit, Show the
  last output, Agent home or Add a special agent step, Show file, Duplicate
  (account), the rail submenu, Turn on/off, Delete. A double click edits; a
  right click opens More; a card drags onto any rail.
- **Special agent features for every workflow.** An account workflow runs
  with an assignment: the page posts `{payload: {assignment}}` to
  `/<id>/run`, so its steps read `{{ $trigger.assignment }}`. It pins to the
  top, left, right or bottom rail like a desktop workflow (below). Last output
  opens a desktop run's output file or selects the run in Recent runs. A desktop
  workflow without an agent step gets **Add a special agent step**, which opens
  it with an `agent.run` step after its last step (read-only permission) to
  fill in and save.
- **Details pane and its log.** The user asked (2026-10-27) that clicking a
  workflow show its log at the bottom right and open a right-hand pane to see
  and manage all of it. Clicking a card (`selectEntry`) now does three things:
  it highlights the card, opens the details pane (`workflowsDetails`, a
  splitter beside the cards under the toolbar; 360 to 440 px at first, then
  the width you drag it to), and sets Recent runs' workflow filter to that
  workflow. If the selected run is not one of the workflow's runs, its newest
  run is selected, so its steps and log fill the bottom right. A workflow that
  has never run gets an empty log and "<name> has not run yet". The pane
  (`renderDetails`) shows the portrait, name, kind and place, a state chip,
  the rail, description, file problem, Run now / Stop and Run with an
  assignment. It also has a button for every action from the More menu: Edit
  steps, Turn on/off, Start or Pause schedule, Pin to rail, Open output file,
  Agent home or Add an agent step, and Show file for desktop workflows;
  Duplicate and Open in browser for account ones; and Delete. Below those are
  the facts (trigger, schedule, next and last run, running step, runs and
  failures, steps, where, rail, file) and the latest output. Buttons look up
  the entry by key when clicked, so a refresh between drawing and clicking
  acts on current data. The pane redraws only when its entry, the busy flag
  or the theme changes (`m_detailsShown`), and keeps its scroll position for
  the same workflow. It closes on × or when its workflow is gone; × also sets
  the run filter back to All workflows. Running from the pane keeps the
  workflow filter: `followRun` resets it only when it names another workflow,
  and Last output does the same.

**Account workflow pins.** The host keeps them with the desktop's pins
(`ui/workflowPins`) under the repository key `@account/workflows`
(`rw::kAccountRepo`, `kAccountRepo` in the package; it has a slash so the
side rail's workflow items accept it, and no repository key starts with `@`).
`repositoryWorkflowAction("@account/workflows", id, ...)` answers `pin`
(`{rail, name, lastStatus}`), `unpin` and `sync` (`{workflows: [{id, name,
lastStatus}]}`); the page sends `sync` after each list load, which stores the
names and last results in `ui/accountWorkflowPins` for the pin tooltips and
rings and unpins workflows the account no longer has. Clicking such a pin
calls the page's `activateSearchHit("run:<id>")`, which runs it like the Run
button (desktop steps still ask first); its menu offers Run now, Open in
Workflows and the rail actions.

## Desktop workflows and special agents

A repository keeps workflows the desktop runs in `.forkmesh/workflows/<id>.json`.
Special agents are such workflows: since 2026-10-22 a special agent is a
workflow whose first **Special agent** step (`agent.run`) holds everything
`.forkmesh/teammates/<id>.json` used to (portrait, specialty, focus,
responsibility, instructions, playbook shortcut, skills and skill files,
permissions, learning journal, low-cost model, run window, relay and
Marketing channels, native workflow). The user asked for the special agents
to become workflows with every quality they had, so the special agent store
(`desktop/src/TeammateStore.*`) reads these files and every surface that shows
special agents (the rail faces, home pages, moods, phones, Marketing pages)
keeps working. Old `.forkmesh/teammates` files still load; saving one writes
its workflow file and removes the old file, and
`tools/migrate_teammates_to_workflows.py --remove-legacy` moves a whole
repository (this repository's 23 agents were moved that way).

| Piece | Where |
| --- | --- |
| File model, validation, step catalog, special agent mapping, run records, rail pins | `desktop/src/RepoWorkflow.*` (test `forkmesh-repo-workflow-tests`) |
| Step-by-step runner (hooks, no window) | `desktop/src/RepoWorkflowRunner.*` |
| Step executors, schedules, events, host actions, rail pins | `desktop/src/MainWindowRepoWorkflows.cpp` |
| Web Repository view kind `workflow` and the Python converter | `app/src/repo_workflows.py`, plugged into `workflow_files.py` |
| Page | the "On this desktop" section of the native Workflows page |

**File.** `{kind: "forkmesh.workflow", version: 1, id, name, description,
icon, enabled, createdAtMs, updatedAtMs, nodes, connections}`; nodes and
connections are the account workflows' shape. The id is the file name.
Limits: 60 steps, 120 connections, no loops, nothing connects into a trigger.

| Step | Type | Does |
| --- | --- | --- |
| Run by hand | `workflow.manual` | Every workflow can run by hand; this only marks it (one at most). |
| Schedule | `workflow.schedule` | The special agent cadences: on demand, continuous, hourly, daily, weekly, weekdays, chosen days, or on repository events (one at most). |
| Log monitor | `workflow.monitor` | An app-log badge threshold that starts a repair run (16 at most). |
| Special agent | `agent.run` | Runs an agent with the role, instructions and only the ticked permissions; its reply is the output. |
| Run shortcut | `workflow.shortcut` | Runs a `.forkmesh/shortcuts` script (`bash`, with `FORKMESH_WORKFLOW_INPUT`, `FORKMESH_WORKFLOW` and `FORKMESH_WORKFLOW_RUN`) or prompt (an agent, Plan unless **may edit files**). **Wait** off starts it and moves on. |
| Save output | `workflow.output` | Writes the run so far as Markdown; a path also saves it in the repository, never under `.git` or `.forkmesh` and never through a link out of the checkout. |
| Note | `workflow.note` | Passes its input on. |
| Desktop steps | `desktop.*` | The six `tools/free_disk_space.py` steps, always the copy in this desktop's own source folder (`forkmesh::sourceDir()/..`), never one from the checkout that triggered the run: a repository could commit its own `tools/free_disk_space.py`. The phone's disk cleanup (`runLocalControlFreeDiskSpace`) uses the same copy. |

**Runs.** `Runner` walks the steps in connection order, one at a time. A step
runs when one of the steps before it succeeded (or failed with **Keep going if
this step fails**) and gets their output: a special agent under "Output of the
earlier workflow steps", a script in `FORKMESH_WORKFLOW_INPUT`. A failure
skips the rest. A workflow runs once at a time and never while turned off.
Each run is kept in `<data>/workflow-runs/<repository digest>/<id>.json` (30
runs, each step's status, time, agent session, output, error and last log
lines) and writes `<id>/<run id>.md`, the run's output file; the Caption line
an agent ends with becomes the run's caption.

**Special agents run as workflow runs.** `runTeammate` (play buttons, phones,
schedules, events, monitors, mentions, Run team) starts the agent's workflow
(`runTeammateWorkflow`). The special agent step still starts through
`startTeammateSession`, so the permission grant, `--disallowedTools`, the
external-call gate, the run mode clamp and the guardrails are unchanged; the
session is the step's runtime, filed in the **Workflows** agent folder. A run
starts a fresh session. The user asked (2026-10-25) for a way back from such an
agent to its workflow: the top of the agent pane shows **← Workflow: <name>**
when the session's special agent id belongs to an agent step of a workflow in
its repository (`rw::runsAgent`: the main agent step runs under the workflow
id, any other under `rw::agentStepId`, `<workflow>-<step>`), and clicking it
opens that workflow in the Workflows page (`openWorkflowInPage`). The lookup
reads the repository's workflow files once per selected session
(`refreshAgentWorkflowLink`); shared team runs have no single workflow and
show no link. A mention or a direct prompt to an agent that already
has a conversation is chat, not a run: it continues that session
(`startTeammateSession`) without a workflow run. Shared "Run all in one session" runs are still
one shared session. Schedules of workflows without an agent step are checked
on the same 60 s tick (`checkRepoWorkflowSchedules`) across every local
repository, and their event triggers fire from `triggerTeammatesForEvent`.

**Approval before a workflow runs by itself.** A workflow file is code: a
shortcut step runs a script with `bash`, and an agent step runs an agent with
the file's instructions. Since 2026-10-27, a schedule or a repository event
starts a workflow or special agent from a checkout only after the user has
approved that file on this desktop. Before then, the author of any repository
that ended up as a checkout (imported, forked, or changed by a merged pull
request) could have run commands as the user within a minute of a repository
page opening.
- **The check.** `rw::approvalDigest` hashes the file's JSON without its
  bookkeeping counters (`withoutBookkeeping`), plus the name and bytes of
  every `.forkmesh/shortcuts` script its shortcut steps run. The digest is
  stored in QSettings under `repoWorkflows/approved/<repository and path
  hash>`. `checkRepoWorkflowSchedules`, `triggerRepoWorkflowsForEvent`,
  `checkTeammateSchedules` and `triggerTeammatesForEvent` call
  `repoWorkflowApproved` before they start anything.
- **When the file changes.** Any change to the file or to a script it runs
  needs approval again.
- **What the user sees.** An unapproved workflow is skipped and raises one
  page notice per file version. Its **Review** button (`fm:workflows:approve`)
  opens an Allow / Not now box that lists the pending files. Allow stores the
  digest the box was opened with, so a file that changed while the box was
  open is not approved.
- **Saving in the editor.** Saving, turning on, or changing the schedule of a
  workflow in the Workflows editor approves the file it writes.
- **What it does not cover.** Runs started by hand (play buttons, pins, the
  page) are the user's own click and are not asked. Mentions in chat are not
  covered either.
- **This repository's special agents.** Their schedules are off in their
  files (only `ux.json` is on), so nothing stops silently. A schedule a user
  turns on outside the editor shows the notice once.

**Rails.** A workflow can be pinned to the top bar, under the left rail, at the
right edge or in the status bar (`ui/workflowPins`, at most 48 pins; one rail
per workflow). A pin shows the workflow's icon (a special agent's portrait), a
blue ring while it runs and a red one after a failure; clicking runs it (or
opens it while it runs), right-click stops it, opens it, its agent's home or
its last output, or moves it. The special agent menu and the page offer
**Pin to rail** too. Right-rail pins show in the desktop's side rail, and a
card on the Workflows page (or a pin on another rail) can be dragged onto the
side rail or any pin strip to pin it there
([../desktop/side-rail-and-terminals.md](../desktop/side-rail-and-terminals.md)).
Account workflows pin the same way ([One list](#one-list-on-the-desktop)).

**Page.** The native Workflows page shows these workflows in its one list
beside the account's ([One list](#one-list-on-the-desktop)), with their icons,
repository, trigger, state, latest output and next run, and Run/Stop, Edit,
Agent home, Pin to rail, schedule and on/off controls under each card; their
runs join Recent runs, with the output file and agent session
one click away. **New workflow** starts a special agent, a shortcut run, a
blank desktop workflow or Free disk space in a chosen repository. The editor
adds checkbox, line-list, portrait and shortcut fields. It needs the
`desktop.workflows.run` permission, which gives the page the host's
`repositoryWorkflows()` and `repositoryWorkflowAction(repo, id, action,
payload, done)` (actions `get`, `save`, `run`, `stop`, `delete`, `pin`,
`unpin`, `setEnabled`, `setSchedule`, `openAgent`, `openSession`,
`openOutput`, `readOutput`, `openFile`; see
[../marketplace/README.md](../marketplace/README.md)).

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
