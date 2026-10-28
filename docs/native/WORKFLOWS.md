# Workflows on iPhone and Android

Part of [the native docs](README.md). The `workflows` plugin
([docs/app/workflows.md](../app/workflows.md)) used to open
`/dashboard/workflows` in the browser on phones. It is now a native screen on
both apps (`Route.workflows`), and a phone can run a workflow itself.

## What the phone does

- **List, create, edit, delete** workflows over `/api/workflows` (the same
  routes the web canvas uses). New workflows start blank or from an example
  (`/api/workflows/templates`).
- **Edit** the graph as an ordered list of steps instead of a canvas: the
  trigger (manual, webhook, schedule), then each step with its inputs ("After
  Fetch", "If: true"). Adding a step after an If asks which branch. A removed
  step's parents are joined to its children. New steps get positions one
  column (288) to the right of their parent, so the web canvas stays readable.
- **Run on ForkMesh**: `POST /api/workflows/<id>/run` (saves first if the
  draft changed), with the server's credentials and limits.
- **Run on this phone**: the phone runs the engine itself and makes every HTTP
  request from the device. The run and its per-step report stay on the phone
  (last 30 per workflow, in memory) and show next to the server runs.
- **Credentials**: the account keeps credentials encrypted and never returns
  their values, so a phone run cannot use them. The Credentials screen lets the
  user save a copy of a credential's values on the phone (iOS Keychain through
  `SecureStore`, Android `SecretStore`, key `workflows.credential.<id>`).
  Creating a credential on the phone keeps a copy by default. A step whose
  credential is not on the phone fails a phone run with the server's own
  message ("Choose a Slack credential for this step"), and the editor lists
  those steps before the run.
- **Run workflow on the phone**: before a phone run the store fetches the
  workflows a Run workflow step can reach (by ID or name from the account
  list; every listed workflow when the reference is an expression), up to the
  nesting limit, and passes them to the engine as `workflows` with the phone
  credentials their steps use. The draft being edited stands in for itself.

## The desktop's workflows

The user asked (2026-10-28) for every workflow the desktop shows to appear on
the phone too, for them to run from the phone, and for Free disk space to run
from the phone and clean the desktop. Before, the phone listed only the
account's workflows (`/api/workflows`), so the desktop's special agents and
repository workflows (`.forkmesh/workflows/*.json`,
[../app/workflows.md](../app/workflows.md#desktop-workflows-and-special-agents))
were missing, and Free disk space could only run from the browser or the
desktop.

- **One list, two sections.** The Workflows screen shows **On ForkMesh** (the
  account's workflows, as before) and **On your desktop**, which
  `DesktopWorkflowsStore` (`FMWorkflows/DesktopWorkflows.swift`) fills from
  the desktop's `workflows.list`. Each row names its kind (Special agent or
  Desktop workflow), repository, state and last run, with a play button.
  Tapping a row opens `DesktopWorkflowScreen`: its facts, Run on the desktop
  (with an optional assignment) or Stop while it runs, its steps with the
  latest run's results, and its last ten runs, each opening the steps' output,
  error and last log lines. The list and an open screen reload on the
  desktop's `workflows.changed` bell, which fires whenever the desktop's
  Workflows page would redraw (`notifyRepoWorkflowsChanged`).
- **The `workflows.` family** (`desktop/src/MainWindowLocalControlWorkflows.cpp`,
  pure half `LocalControlWorkflows.{h,cpp}`, test
  `forkmesh-local-control-workflows-tests`), owner-only like every family:
  `list` (the Workflows page's `nativePackageRepositoryWorkflows()`, without
  portraits, local paths or runs; at most 200 rows), `get` (`repo`, `id`: the
  steps' ids, types and names and the last ten runs, bounded, without the
  assignment or output file path), `prepare` and `run` for the actions `run`
  and `stop`. `run` needs the token `prepare` issued to the same device for
  the same `repo|id`, like `teammates.`, then calls the page's own
  `runNativePackageRepositoryWorkflowAction`, so a phone run is the same as
  pressing Run on the desktop (trigger "By hand · <phone>"). Editing and
  deleting these workflows stays on the desktop. A phone `run` of a
  repository workflow needs the workflow file's approval on the desktop
  (`approval_required` otherwise), and fails closed with `unknown_workflow`
  when the desktop cannot find the repository or the workflow file, so no
  lookup gap skips the approval check (guard:
  `desktop/tests/validate_repo_workflow_approval.py`).
- **Free disk space from the phone.** An account workflow with desktop steps
  (`desktop.*`, `WorkflowDesktopSteps`, checked against `workflow-spec.json`
  in `DesktopWorkflowsTests`) shows **Run on your desktop** instead of the
  phone and ForkMesh buttons, which refuse those steps. After a confirmation
  naming the steps, `WorkflowsStore.runOnDesktop` does what the web's
  `wfRunOnDesktop` does: steps in graph order, each desktop step through
  `diag.prepare` and `diag.run` with action `freeDiskSpace` (20 minutes a
  step), a failure skips the rest, and the report goes to
  `POST /api/workflows/<id>/record`, so the run joins the workflow's history
  as a desktop run. The step list shows each step's progress live
  (`desktopLive`). The desktop runs `tools/free_disk_space.py` in its own
  forkmesh/forkmesh checkout; nothing new runs on the desktop for this.

## One engine, three runtimes

| Runtime | Code | How it is checked |
| --- | --- | --- |
| Server (reference) | `app/src/workflows.py` | `app/tests/test_workflows.py` |
| iPhone: Apple's JavaScriptCore | `marketplace/packages/workflows/content/mobile/workflow-engine.js`, hosted by `FMWorkflows/WorkflowScript.swift` (`JSContext`) | Node in `marketplace/packages/workflows/tests/`; JSC in `FMWorkflowsTests` (macOS) |
| Android | `core/.../workflows/WorkflowEngine.kt` (+ `WorkflowPy.kt`) | `WorkflowsTest` (`:core:coreTest`) |

The JavaScript engine is a line-for-line port of `workflows.py`: validation
(`clean`), expressions, every core node including the scripting, text,
number, list and date steps ([app/workflow-steps.md](../app/workflow-steps.md)),
the script interpreter ([app/workflow-scripts.md](../app/workflow-scripts.md)),
integration request building, the run report. Python details that change results are reproduced on purpose:
`json.dumps` spacing and `ensure_ascii`, `quote`/`urlencode` escaping, Python
truthiness for `or`, `str(True)`, negative list indexes, the lazy whole-value
expression match.

Android has no JavaScriptCore, the project allows first-party dependencies
only (`docs/native/README.md`), and the offline build cannot add QuickJS or
`androidx.javascriptengine`, so Android runs a Kotlin port of the same code.

**HTTP stays native.** The JS engine is a generator: `start(input)` returns
either `{request}` or `{done: report}`; the host performs the request with its
own HTTP stack and calls `resume(session, {status, text})`. A Wait step
yields `{wait: ms}`: the host sleeps (iOS `Task.sleep`, Android the `sleep`
callback of `WorkflowEngine.run`) and resumes with `null`. Random numbers come
from the platform unless the input carries `randoms`, which the conformance
cases use. JavaScriptCore has
no `fetch`, and this keeps the JSContext single-threaded behind a lock while
the request runs on URLSession (`HTTPWorkflowTransport` over FMNet's
`HTTPClient`). Android's `HttpWorkflowTransport` uses `HttpURLConnection`.

**Same guards as the server.** Every request and every redirect hop is checked
with the engine's `safeUrl` (public `https://` only, no IP literals outside
global space, no `localhost`, `.local`, `.internal` or `forkmesh.com`).
Redirects are followed by hand (at most five; 301/302 POST and 303 become GET)
so each hop is checked. Limits come from the server: manual runs get 25
requests in 45 s, 100 items per step, 30 s per request, 256 K characters per
response, the `ForkMesh-Workflows/1.0` user agent. The phone cannot see what a
name resolves to, so a public name pointing at a private address is not
caught on the phone the way the server's egress guard catches it; the phone
reaches only what the phone itself could already reach.

## The generated bundle

`python3 marketplace/tools/workflows_mobile_bundle.py` writes:

- `workflow-spec.json`: core nodes, the 100 integrations, operators, limits
  and run limits, exported from `app/src` (iOS `FMWorkflows/Resources`,
  Android `core/src/main/resources/workflows`). Its `version` is a hash.
- a copy of `workflow-engine.js` into `FMWorkflows/Resources` (SwiftPM
  resources must live inside the target; `Module.assets` in `Package.swift`
  copies the folder).
- `workflow-cases.json`: about 970 runs computed by the server engine (every
  integration operation with sample values and credentials, every example
  template, core flow, expression, URL-safety and error cases, every new step
  with its edge cases, time zones across clock changes, and every program of
  the script corpus plus script errors and limits), plus
  `clean`, `safeUrl` and catalog cases. Both phone test suites replay them and
  compare requests, recorded waits and reports (step timings excluded; script
logs and `stopped` included).

`--check` fails when any file is stale; `test_workflows_mobile.py` runs the
check and the Node conformance harness, so a change to the server engine or an
integration must be followed by a regenerate, and any behaviour it changes
shows up as a failing phone case.

## Code map

| Piece | iOS | Android |
| --- | --- | --- |
| Engine host / port | `ForkMeshCore/Sources/FMWorkflows/WorkflowScript.swift`, `WorkflowEngine.swift` | `core/.../workflows/WorkflowEngine.kt`, `WorkflowPy.kt` |
| Models, API, phone credentials | `WorkflowModels.swift`, `WorkflowsStore.swift` | `core/.../workflows/Workflows.kt` |
| Desktop workflows, desktop steps | `DesktopWorkflows.swift`; screens `DesktopWorkflowScreens.swift` | not yet (`docs/native/SYNC.md`) |
| Screens | `ForkMesh/Features/Workflows/` | `app/.../feature/workflows/` |
| Route | `Route.workflows`, `RouteFallbacks.swift` | `Route.Workflows`, `WorkflowsFeature` |

`/dashboard/workflows` links now parse to the native route on both apps. The
editor's menu still offers the web canvas (`HubItem.workflowsPath`).
