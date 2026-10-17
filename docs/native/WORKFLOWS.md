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

## One engine, three runtimes

| Runtime | Code | How it is checked |
| --- | --- | --- |
| Server (reference) | `app/src/workflows.py` | `app/tests/test_workflows.py` |
| iPhone: Apple's JavaScriptCore | `marketplace/packages/workflows/content/mobile/workflow-engine.js`, hosted by `FMWorkflows/WorkflowScript.swift` (`JSContext`) | Node in `marketplace/packages/workflows/tests/`; JSC in `FMWorkflowsTests` (macOS) |
| Android | `core/.../workflows/WorkflowEngine.kt` (+ `WorkflowPy.kt`) | `WorkflowsTest` (`:core:coreTest`) |

The JavaScript engine is a line-for-line port of `workflows.py`: validation
(`clean`), expressions, every core node, integration request building, the
run report. Python details that change results are reproduced on purpose:
`json.dumps` spacing and `ensure_ascii`, `quote`/`urlencode` escaping, Python
truthiness for `or`, `str(True)`, negative list indexes, the lazy whole-value
expression match.

Android has no JavaScriptCore, the project allows first-party dependencies
only (`docs/native/README.md`), and the offline build cannot add QuickJS or
`androidx.javascriptengine`, so Android runs a Kotlin port of the same code.

**HTTP stays native.** The JS engine is a generator: `start(input)` returns
either `{request}` or `{done: report}`; the host performs the request with its
own HTTP stack and calls `resume(session, {status, text})`. JavaScriptCore has
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
- `workflow-cases.json`: 346 runs computed by the server engine (every
  integration operation with sample values and credentials, every example
  template, and core flow, expression, URL-safety and error cases), plus
  `clean`, `safeUrl` and catalog cases. Both phone test suites replay them and
  compare requests and reports (step timings excluded).

`--check` fails when any file is stale; `test_workflows_mobile.py` runs the
check and the Node conformance harness, so a change to the server engine or an
integration must be followed by a regenerate, and any behaviour it changes
shows up as a failing phone case.

## Code map

| Piece | iOS | Android |
| --- | --- | --- |
| Engine host / port | `ForkMeshCore/Sources/FMWorkflows/WorkflowScript.swift`, `WorkflowEngine.swift` | `core/.../workflows/WorkflowEngine.kt`, `WorkflowPy.kt` |
| Models, API, phone credentials | `WorkflowModels.swift`, `WorkflowsStore.swift` | `core/.../workflows/Workflows.kt` |
| Screens | `ForkMesh/Features/Workflows/` | `app/.../feature/workflows/` |
| Route | `Route.workflows`, `RouteFallbacks.swift` | `Route.Workflows`, `WorkflowsFeature` |

`/dashboard/workflows` links now parse to the native route on both apps. The
editor's menu still offers the web canvas (`HubItem.workflowsPath`).
