# Workflows (visual automation builder)

Part of [the App docs](README.md). A workflow is an n8n-style automation: one
trigger (manual, webhook or schedule) followed by steps on a canvas at
`/dashboard/workflows`. Each step is a core node (HTTP request, Set fields, If,
Filter, Merge, Limit, Split out, No operation), one of the Shortcuts-style
scripting, text, number, list and date steps including Run script
([workflow-steps.md](workflow-steps.md)),
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
  `case1`-`case3` and `otherwise`, others `main`. Limits: one trigger, 60
  nodes, 120 connections, no cycles, nothing into the trigger.
- **Execution** runs in topological order. A node concatenates items from all
  inputs and is skipped if none arrived. A step error stops the run unless
  `continueOnFail` (then it emits `{"error": ...}`). 100 items per node. A
  Stop workflow step ends the run as a success and the report names it in
  `stopped`. Run script steps add their `console.log` lines to the report as
  `logs`.
- **Expressions** `{{ ... }}` are paths, never code: a root (`$json`, `$node`,
  `$trigger`, `$now`, `$today`, `$item`, `$vars`, `$workflow`) then `.name`,
  `[0]` or `["key"]`. A parameter that is one expression keeps its type.
  `$node["Name"].json` is that step's item at the same index.
- **Credentials** are created per integration (`/api/workflows/credentials`)
  and referenced by id; a step uses one only if owner and integration match.
- **Integrations** are `APP(...)` entries (base URL, auth scheme, operations
  with `{field}` path placeholders and optional body template). Chat
  operations add a top-level `text` with the reply. Adding one is one entry;
  the tests check every operation and pin the count.
- **Repository files**: the Repository view edits `.forkmesh/*.yml` actions,
  `.forkmesh/shortcuts/*`, `.forkmesh/skills/*.skill`,
  `.forkmesh/workflows/<id>.json` desktop workflows
  ([below](#desktop-workflows-and-special-agents)) and legacy
  `.forkmesh/teammates/<id>.json` special agents on the canvas. Saving renders
  on the server (`/api/workflows/files/render`), reports warnings, and commits
  through `POST /api/repo/<owner>/<repo>/file-edit` with the read blob oid (a
  concurrent change is a 409). Packages can ship such files through the
  `content.*` sockets ([package system](../marketplace/package-system.md#protocols-and-interfaces)).
  A shortcut script's body is split into blocks on blank lines. A block whose
  first non-empty line is `# step: <name>` shows that name (at most 80
  characters) instead of "Commands N". A block with a line
  `bash .forkmesh/shortcuts/<file> ...` becomes a `shortcut.run` node ("Run
  shortcut", blue) reading "Runs <file>", whose Open link loads that shortcut;
  the first such line wins. The desktop and phone apps parse the same rules.
  Only the block text is saved, so the file round-trips byte for byte (a
  missing final newline is kept in `meta.finalNewline`).
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
| `GET`/`POST /api/workflows` | List / create `{name, active, nodes, connections}`; a listed workflow with desktop steps carries `runsOn: "desktop"`, so the phones can offer Run on your desktop on its row |
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

Selecting a step opens a **What this step is** section above its settings
(`wfAboutHtml`):

- **Kind** (`wfNature`): Trigger, Script, Part of a script (a desktop step,
  an Action's Run step, or a block of a `.sh`/`.bash` shortcut), Integration
  call, Web request, Runs another workflow, Built-in step, Note, or in the
  Repository view Part of a prompt, Part of a skill, Agent setting and File
  header. Scripts and parts of scripts carry a tag on the canvas too.
- **Facts**: type id, where it runs, and linked input and output steps (and
  the workflow or shortcut a Run workflow or Run shortcut step opens).
- **Integrations** show the request with settings filled in, the credential
  and how it is sent (secret values never appear), and under **What it
  sends** the headers, query and body as `build_app_request` builds them. The
  templates come from the catalog's `requests` map, kept out of
  `engine.catalog()` so the phone engines' catalog parity is unchanged.
- **Settings**: every field's value in words, then the saved JSON.
- **Script area.** Desktop steps name `tools/free_disk_space.py`, the
  `--step`, the exact command, the dry run, the desktop function that starts
  it and the script functions it uses (`workflow_desktop.FUNCTIONS`, in
  `script` on each catalog node); **View script** shows them with line
  numbers from `/api/workflows/desktop-script`, which parses the server's
  copy with `ast` (the desktop runs the copy in its own checkout). Run script
  shows its size, mode, code and last log. A repository-file block shows its
  place (block N of M); **View in the whole file** renders the file through
  `/api/workflows/files/render` and marks that step's lines.

## Run output

Every run has an `output` `{text, metrics: [{label, value}]}`, stored in the
run report and returned by `/runs`, so the recent runs list shows it on each
row. A server run's output is `workflows.run_output`: the item count of its
last steps (the steps nothing connects onward from, or the Stop step) and a
preview of the first item (its `output`, `text`, `message`, `result` or
`status` field, else its first three plain fields). A failed run's output is
its error. A desktop run's output is the bytes freed and the free space after
(`workflow_desktop.disk_output`).

### Send a failure to an agent

A failed run shows **Send to agent** in the progress rail's headline, the
Selected execution panel and the failed step's Last run section. It puts a
report in the agent prompt ([web-composer.md](web-composer.md)) through
`sendTextToPrompt`; on a page without a prompt it copies the report instead
(`wfSendFailure`). The report (`wfFailurePrompt`) names the workflow and its
link, the run and its error, every step with its type, status, time and
error, and for the failed step:

- a desktop step: the `tools/free_disk_space.py --step <step>` command, the
  dry run that repeats it, and the docs to read;
- any other step: its settings as JSON (at most 1500 characters), with values
  whose key, or whose pair's `name`, looks like a token, secret, password, API
  key, auth header, cookie or signature replaced by `[hidden]`;
- its last 40 log lines, if any.

The report stops at 7500 characters, under the prompt's 8000. A failing
desktop step sends only the last line it printed (the desktop's
`cleanup_failed` message), so that is the error the report carries.

## Desktop steps

The **Desktop** group (`workflow_desktop.NODES`, `runsOn: "desktop"`) holds
steps only the user's own desktop can run: the six parts of
`tools/free_disk_space.py` ([free-disk-space.md](../tools/free-disk-space.md)),
one step type each so each has its own icon.

- **Who runs them.** The server and phone engines fail such a step with its
  `refusal` text, and `POST /<id>/run` answers `runs_on_desktop`. The
  browser's **Run workflow** (`wfRunOnDesktop`) and the iPhone's **Run on
  your desktop** ([native/WORKFLOWS.md](../native/WORKFLOWS.md#the-desktops-workflows))
  reach the first linked desktop through `desktopControlHub()`
  ([agents-page.md](agents-page.md#desktop-agents-on-a-repositorys-agents-tab)), send `diag.prepare` and
  `diag.run` (action `freeDiskSpace`) per step in graph order, wait up to 20
  minutes a step, and post the report to `/<id>/record`, which checks every
  node id, status and duration (`workflow_desktop.clean_report`) and stores a
  `desktop` run.
- **Rules.** Such a workflow starts from Manual run and holds only desktop
  steps besides No operation and notes (`desktop_needs_manual`,
  `desktop_steps_only`): schedules and webhooks have no browser to drive
  them, and server steps cannot run in the same pass.
- **Preinstalled.** Listing workflows (`_list`) adds **Free disk space**
  (Manual run and the six steps) to an account that never had it, under an id
  derived from the account (`seed_id`). Deleting it leaves an ownerless empty
  row under that id so it is not re-added, with no new table.
- **Progress rail.** `wfRenderRail` draws a bar as wide as the longest
  recorded run, marking each other run's duration. A live run fills it and
  slides the running step's icon along (redrawn every 250 ms); finished
  steps stay where they ended, ringed green or red.

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

The desktop has a native page ([On the desktop](#on-the-desktop)). The phones
list the desktop's repository workflows and special agents and run them
through the desktop's `workflows.` control family. On iPhone and Android the
plugin is a native screen and a phone can run a workflow itself: the
package's `content/mobile/workflow-engine.js` runs in JavaScriptCore on
iPhone, Android runs a Kotlin port, and both replay cases produced by this
engine. After changing `workflows.py`, `workflow_integrations.py` or the run
limits, run `python3 marketplace/tools/workflows_mobile_bundle.py` and keep
the phone engines passing ([native/WORKFLOWS.md](../native/WORKFLOWS.md)).

## On the desktop

**Workflows** on the desktop rail opens a native page (`builtin:workflows`,
API prefix `/api/workflows`, proof `forkmesh-workflows-request-v1`). It calls
the web canvas's routes through the host's `request`, which sends the account
session as a Bearer token or, when the desktop holds no session for the
server, signs the request with the account key (`node`, `ts`, `sig` over the
proof, method, path and the body's SHA-256). `_session_account` falls back to
`_plugin_scope_signed_session` with `REQUEST_PROOF`, as Notes does, so a
desktop without a session still lists its workflows. A 401 or 403
`plugin_not_installed` shows as a sentence in the status line (`errorText`
maps every server error code).

- **Dashboard.** Stat tiles (workflows and steps, active, runs today,
  success rate, average duration, last run and last failure, next scheduled
  run) over **one list** of every workflow ([One list](#one-list-on-the-desktop)).
  Recent runs merges the 30 kept runs of every workflow with a `lastRunAt`
  (`GET /<id>/runs`, at most four at a time), newest first, filtered by
  workflow and result. Selecting one shows its steps as an animated chain
  (`RunFlow`, `workflowsRunFlow`) above the output (`workflowsRunOutput`):
  each step's output, logs and error, then a per-step table of result, time,
  items and first item or last log line.
- **Run goes to Recent runs.** Run, Run with an assignment, a rail pin and
  the editor's Run workflow follow the new run (`followRun`): filters reset
  (unless the workflow filter already names it), the runs pane grows to at
  least 40% and the run is selected. A desktop run is matched by the `runId`
  the `run` action returns, else by the first run of that workflow started
  within two minutes. Every Run at once adds a "Running" placeholder
  (`m_pendingRun`, `startPendingRun`) with the steps as Waiting
  (`pendingSteps`); an account run replaces it with the run `POST /<id>/run`
  returns (`landPendingRun`), a desktop run once a list holds its `runId`
  (dropped after two minutes otherwise), and desktop steps fill it as they
  start and end (`updateDesktopPending`).
- **Run animation.** When the followed run's row first appears,
  `RunSparkle::burst` overlays a 1.4 s flash, sweep and stars on that row
  (found by run id each frame). `RunFlow` draws one circle per step: Waiting
  numbered, Running spinning with a halo, and a check, cross, dash or square
  for Succeeded, Failed, Skipped and Stopped; a line fills green after a
  success. A live run with no running step (an account run answers only when
  finished) runs a light along the chain, or says Starting with no steps.
  Steps changing in one update turn 160 ms apart; steps of the same run, or
  of the run that replaced the placeholder (`m_flowAliasFrom`/`m_flowAliasTo`),
  keep their state and animate only what changed. The timer runs at 30 fps
  only while something moves or a run is live and the page is visible. The
  chain scrolls sideways past about 104 px a step, keeping the running step
  in view. While a desktop run is live the page polls the host's `get` (the
  active run) every second; the runner fills a running step's `output` and
  `logs` (an agent's latest reply, a shortcut's output so far) in memory
  only, and writes the run file when a step starts or ends.
- **Editor.** The graph is an ordered list of steps like the phones'
  (`Draft::ordered`, Kahn order); the form is built from the catalog fields
  (json and code fixed-width). Integrations add Operation (switching keeps
  shared values) and Credential with **Manage...**. Every non-trigger step
  has **Runs after**: its inputs with Disconnect, and Connect for any step
  and branch that makes no loop (`Draft::canConnect`), so Merge takes several
  inputs. A new step goes one column (288) right of its parent and one row
  (120) down per sibling; removing a step joins its parents to its children;
  changing the trigger keeps its id and connections; a trigger added to a
  graph without one feeds every unconnected step. Values keep their JSON type
  while the edit parses. Webhook workflows show their URL with Copy and New
  URL (`rotateHook`). Save posts the whole graph, Run workflow saves first,
  Download writes the `/export` n8n file, and leaving unsaved asks Save,
  Discard or Cancel.
- **Editor layout.** Three columns: steps, the selected step's form, and
  runs (table above the selected run's steps). Each step is a card
  (`StepDelegate` in `WorkflowsCards`) with a numbered badge coloured by kind
  (trigger accent, special agent purple, desktop step teal, else grey, dashed
  when off), name, type, summary, what it runs after ("Not connected" in the
  attention colour) and a status dot for the latest run; the item text keeps
  all three lines for accessibility. Chips under the name bar
  (`workflowsFact`, `renderEditorFacts`) give state, trigger, next and last
  run, success share, step count and where the workflow lives. Checkbox
  groups over four options use four columns. A desktop run's step table
  names its fourth column Session rather than Items.
- **New workflow.** Blank, an example from `/templates`, **Import file...**
  (`/convert`, up to 2 MB) or **Import from link...** (`/import-url`); all
  open unsaved and off with conversion warnings. **Credentials** lists masked
  credentials, adds and deletes them.
- **Desktop steps run here.** After a confirmation naming each step, the page
  finds a repository with `tools/free_disk_space.py` through the host's
  `repositories` (`repository.read` permission; `forkmesh/forkmesh` first)
  and runs `python3 -u tools/free_disk_space.py --execute --step <step>
  --repo <checkout> --json` per step, the local-control runner's command
  (`freeDiskSpaceArguments`), with the same 20-minute limit and skip rules as
  `wfRunOnDesktop`. The process (`forkmeshFreeDiskSpaceProcess`) is parented
  to the page's window, so it and a browser-driven run refuse to overlap. The
  report goes to `/<id>/record`.
- **Search and badge.** The top search box finds workflows by name; the rail
  badge counts workflows whose last run failed.

### One list on the desktop

Special agents, repository desktop workflows and account workflows share one
list, and every workflow gets the special agent features.

- **Entries.** `unifiedWorkflows` (`WorkflowsModel.cpp`) gives both sources
  one shape: `key` (`local:<repo>|<id>` or `account:<id>`), `source`, `kind`
  (`agent`, `desktop`, `account`; labels Special agent, Desktop workflow, On
  ForkMesh), place, trigger, state, schedule, steps, run history, next run,
  rail pin and latest output (`lastSummary` and output file, or the newest
  account run's `runOutputText`). `statsWorkflows` feeds the stat tiles.
- **Toolbar.** Kind chips (All, Special agents, On this desktop, On
  ForkMesh, Pinned, Needs attention) and the filter box (`workflowMatches`:
  name, description, trigger, repository, kind, output); sort by name, last
  run, next run or kind (`sortWorkflows`); List, Thumbnails or Compact grid.
  View and sort persist in `workflows/viewMode` and `workflows/sort`.
- **Cards** (`WorkflowsCards.*`). Thumbnails: a header tinted by kind (violet
  special agents, blue desktop, teal ForkMesh) with icon or portrait, a small
  drawing of the steps, state pill and rail, then name, kind, place, trigger,
  two lines of output and run times. Compact grid: one line each. List: one
  row per workflow. Cards flow into as many columns as fit (`CardGrid`;
  270 px thumbnails, 210 px compact).
- **Controls.** Each card has Run/Stop, Edit, Pin to rail, Start or Pause
  schedule, Last output, an on/off switch and More (adds Run with an
  assignment, Agent home or Add a special agent step, Show file, Duplicate
  for account workflows, Delete). Double click edits, right click opens More,
  and a card drags onto any rail.
- **Special agent features for every workflow.** An account workflow runs
  with an assignment: the page posts `{payload: {assignment}}` to
  `/<id>/run`, so its steps read `{{ $trigger.assignment }}`. It pins to any
  rail like a desktop workflow. Last output opens a desktop run's output file
  or selects the run in Recent runs. A desktop workflow without an agent step
  gets **Add a special agent step**, which opens it with an `agent.run` step
  after its last step (read-only permission) to fill in and save.
- **Details pane.** Clicking a card (`selectEntry`) highlights it, opens the
  details pane (`workflowsDetails`, a splitter beside the cards, 360 to 440 px
  at first) and filters Recent runs to it, selecting its newest run so its
  steps and log fill the bottom right ("<name> has not run yet" if none). The
  pane (`renderDetails`) shows identity, state, rail, description, file
  problem, Run now / Stop, Run with an assignment, a button for every More
  action, the facts and the latest output. Buttons look up the entry by key
  when clicked, so a refresh in between acts on current data. The pane
  redraws only when its entry, the busy flag or the theme changes
  (`m_detailsShown`). × closes it and resets the run filter to All
  workflows; `followRun` and Last output reset the workflow filter only when
  it names another workflow.

**Account workflow pins.** The host keeps them with the desktop's pins
(`ui/workflowPins`) under the repository key `@account/workflows`
(`rw::kAccountRepo`, `kAccountRepo` in the package; it has a slash so the
side rail's workflow items accept it, and no repository key starts with `@`).
`repositoryWorkflowAction("@account/workflows", id, ...)` answers `pin`
(`{rail, name, lastStatus}`), `unpin` and `sync` (`{workflows: [{id, name,
lastStatus}]}`); the page sends `sync` after each list load, which stores
names and last results in `ui/accountWorkflowPins` for the pin tooltips and
rings and unpins workflows the account no longer has. Clicking such a pin
calls the page's `activateSearchHit("run:<id>")`, which runs it like the Run
button (desktop steps still ask first); its menu offers Run now, Open in
Workflows and the rail actions.

## Desktop workflows and special agents

A repository keeps workflows the desktop runs in
`.forkmesh/workflows/<id>.json`. A special agent is such a workflow whose
first **Special agent** step (`agent.run`) holds everything
a `.forkmesh/teammates/<id>.json` file held (portrait, specialty, focus,
responsibility, instructions, playbook shortcut, skills and skill files,
permissions, learning journal, low-cost model, run window, relay and
Marketing channels, native workflow). The special agent store
(`desktop/src/TeammateStore.*`) reads these files, so every surface that shows
special agents (rail faces, home pages, moods, phones, Marketing pages) keeps
working. Old `.forkmesh/teammates` files still load; saving one writes its
workflow file and removes the old file, and
`tools/migrate_teammates_to_workflows.py --remove-legacy` moves a whole
repository.

| Piece | Where |
| --- | --- |
| File model, validation, step catalog, special agent mapping, run records, rail pins | `desktop/src/RepoWorkflow.*` (test `forkmesh-repo-workflow-tests`) |
| Step-by-step runner (hooks, no window) | `desktop/src/RepoWorkflowRunner.*` |
| Step executors, schedules, events, host actions, rail pins | `desktop/src/MainWindowRepoWorkflows.cpp` |
| Web Repository view kind `workflow` and the Python converter | `app/src/repo_workflows.py`, plugged into `workflow_files.py` |
| Page | the native Workflows page |

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
runs when one of the steps before it succeeded (or failed with **Keep going
if this step fails**) and gets their output: a special agent under "Output of
the earlier workflow steps", a script in `FORKMESH_WORKFLOW_INPUT`. A failure
skips the rest. A workflow runs once at a time and never while turned off.
Each run is kept in `<data>/workflow-runs/<repository digest>/<id>.json` (30
runs, each step's status, time, agent session, output, error and last log
lines) and writes `<id>/<run id>.md`, the run's output file; the Caption line
an agent ends with becomes the run's caption.

**Special agents run as workflow runs.** `runTeammate` (play buttons, phones,
schedules, events, monitors, mentions, Run team) starts the agent's workflow
(`runTeammateWorkflow`). The agent step still starts through
`startTeammateSession`, so the permission grant, `--disallowedTools`, the
external-call gate, the run mode clamp and the guardrails are unchanged; each
run is a fresh session filed in the **Workflows** agent folder. The agent pane
shows **← Workflow: <name>** (`refreshAgentWorkflowLink`, `rw::runsAgent`; the
main agent step runs under the workflow id, others under `rw::agentStepId`,
`<workflow>-<step>`) and opens it with `openWorkflowInPage`; shared team runs
show no link. A mention or direct prompt to an agent with a conversation
continues that session without a workflow run. Schedules of workflows without
an agent step are checked on the same 60 s tick
(`checkRepoWorkflowSchedules`) across every local repository; their event
triggers fire from `triggerTeammatesForEvent`.

**Approval before a workflow runs by itself.** A workflow file is code: a
shortcut step runs a script with `bash`, and an agent step runs an agent with
the file's instructions. A schedule or repository event starts a workflow or
special agent from a checkout only after the user has approved that file on
this desktop; otherwise the author of any repository that became a checkout
(imported, forked, or changed by a merged pull request) could run commands as
the user within a minute of a repository page opening.

- **The check.** `rw::approvalDigest` hashes the file's JSON without its
  bookkeeping counters (`withoutBookkeeping`), plus the name and bytes of
  every `.forkmesh/shortcuts` script its shortcut steps run. The digest is
  stored in QSettings under `repoWorkflows/approved/<repository and path
  hash>`. `checkRepoWorkflowSchedules`, `triggerRepoWorkflowsForEvent`,
  `checkTeammateSchedules` and `triggerTeammatesForEvent` call
  `repoWorkflowApproved` before they start anything.
- **When the file changes.** Any change to the file, or to a
  `.forkmesh/shortcuts` script one of its shortcut steps names, needs
  approval again. Programs and files those scripts use in turn are not in
  the digest, and the review box says so.
- **What the user sees.** An unapproved workflow is skipped and raises one
  page notice per file version; **Review** (`fm:workflows:approve`) opens an
  Allow / Not now box. Allow stores the digest the box opened with, so a file
  that changed meanwhile stays unapproved.
- **Saving and toggling.** The editor's `save` approves a file only when it
  is new or differs from the committed file, so a package cannot approve a
  stranger's workflow by saving it back. `setEnabled` and `setSchedule`
  carry an existing approval to the new digest; an unapproved file stays
  unapproved and turning it on raises the notice.
- **Runs from a package.** The package `run` action is not the user's click,
  so an unapproved repository workflow answers `code: approval_required` and
  raises the notice. The `review` action opens the box and answers when it
  closes; only the user's click in the desktop's own box approves.
- **Run on the Workflows page.** On `approval_required` the placeholder
  waits ("waiting for your approval"), `review` opens the box and Allow
  starts the run. A Run that does not start for any reason, or an account run
  `POST /<id>/run` refuses, becomes a Failed row with the reason
  (`failPendingRun`) rather than vanishing. A special agent that fails at
  once returns the failed run's `runId`, so the page selects it.
- **From the phone.** `insights` `runShortcut` / `queueShortcut` (committed
  `.forkmesh/shortcuts` and `.forkmesh/skills`; a non-JSON file's digest is
  SHA-256 over its bytes) and `workflows.run` answer `approval_required`
  until the file is approved on this desktop. The phone sees only a name, so
  its confirmation is not a code review. Account workflows are not asked.
- **Not covered.** The desktop's own play buttons and pins, and chat
  mentions.
- **This repository's special agents** have their schedules off (only
  `ux.json` is on), so nothing stops silently.

**Rails.** A workflow pins to the top bar, under the left rail, the right edge
or the status bar (`ui/workflowPins`, at most 48 pins, one rail each). A pin
shows the icon or portrait with a blue ring while running and red after a
failure; click runs it (or opens it while running), right-click stops, opens,
shows agent home or last output, or moves it. Right-rail pins show in the
side rail, and cards or pins drag onto any pin strip
([side-rail-and-terminals.md](../desktop/side-rail-and-terminals.md)).

**Page.** These workflows share the native page's list and Recent runs.
**New workflow** starts a special agent, a shortcut run, a blank desktop
workflow or Free disk space in a chosen repository; the editor adds checkbox,
line-list, portrait and shortcut fields. The `desktop.workflows.run`
permission gives the page `repositoryWorkflows()` and
`repositoryWorkflowAction(repo, id, action, payload, done)` (actions `get`,
`save`, `run`, `review`, `stop`, `delete`, `pin`, `unpin`, `setEnabled`,
`setSchedule`, `openAgent`, `openSession`, `openOutput`, `readOutput`,
`openFile`; [package system](../marketplace/package-system.md#desktop-page-contract)).

## Security

- Definitions, run reports and credentials are encrypted (`encrypt_row`);
  credential values are never returned.
- Steps call only public `https://` hosts (no IP literals outside global
  space, no internal names, no user info); the host egress guard re-checks
  every connection and redirect.
- Webhook tokens are 256 random bits compared with `hmac.compare_digest`,
  valid only while the workflow is active with a webhook trigger.
- Run script code is interpreted by `workflow_script.py`, a sandboxed
  JavaScript subset with no access to the host, files or the network, a step
  limit, a call-depth limit and size caps ([workflow-steps.md](workflow-steps.md#run-script)).
  Nothing a user types reaches `eval`, `exec` or a JavaScript engine.

## Web styling

`view.html` styles the page with its own `wf-*` classes but takes every
colour and size from the dashboard theme tokens
([dashboard-colors.md](dashboard-colors.md)). `.wf-btn` is the 32px control,
`.wf-btn-primary` is the green create colour (Run workflow, Add first step,
Import), the active toggle is blue, and status dots use the success,
destructive and muted tokens. The File menu is a popover with 30px rows and
muted icons, `.wf-notice` tints its top bar with the success, destructive or
attention colour from `data-tone`, dialogs use the shell dialog radius and
shadow, and group headings are sentence case.

## Traps

`/dashboard/workflows` must not be in `dashboard_shell.PAGES` (it stays in
`CHUNK_PAGES` / `LAZY_CHUNKS` and the `builtinPage` allowlists in
`10-boot.js`). The runs column is `trigger_kind`. Locally
`plugin_registry.py` is empty, so every package counts as installed.

The `actions-schedule` cron phase (every minute) is a different mechanism:
it queues `.forkmesh/*.yml` workflows with an `on.schedule` cron on an
instance's own runner ([instance-actions.md](../server/instance-actions.md)).
