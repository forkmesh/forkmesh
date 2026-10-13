# Mirror Actions control and summary v2

The website's repository **Actions** tab reads run state from, and sends
control requests to, a mirror node that runs the Actions executor. This page is
the wire contract shared by the four components involved:

| Component | File |
| --- | --- |
| Website | `app/public/dashboard/js/06-repo-actions.js` |
| Relay Worker | `app/src/repository_actions.py` |
| Mirror gateway | `tools/mirror_gateway.py` |
| Executor (Go mirror daemon) | `server/actions.go`, `server/actions_control.go`, `server/actions_workflow.go` |

The mirror fleet runs the Go daemon (`forkmesh-mirror-node`), which supervises
this same Python gateway. The daemon is the executor: it runs the workflows,
writes the summary, and consumes control requests. A desktop that publishes its
own runs (the existing `--configure-mirror-actions-stdin` path) still writes a
version 1 summary, which every reader keeps accepting read-only.

Nothing here can turn on an executor its operator turned off.

## Enabling Actions on a mirror

Running Actions means running shell a repository supplies, so it is opt-in per
node and fails closed:

1. Deploy the current `tools/mirror_gateway.py` **and** the current daemon
   binary. The daemon checks that its gateway script knows the Actions keys
   before it names them: an older gateway refuses to start on a configuration
   key it does not know, which would take down the node's whole gateway.
2. Install `server/packaging/systemd/forkmesh-mirror-node-actions.conf` as
   `/etc/systemd/system/forkmesh-mirror-node.service.d/actions.conf`. The base
   unit's `RestrictNamespaces=true` forbids the unprivileged namespaces
   bubblewrap builds its sandbox from; the drop-in allows exactly those.
   `bubblewrap` must be installed.
3. Set `"enableActions": true` in `mirror-node.json` and restart the daemon.

At start the daemon builds a throwaway sandbox. If that fails it logs
`mirror actions are unavailable: …`, runs nothing, and strips `actions-status`
and `actions-control` from every repository. It never falls back to running a
step unsandboxed. When the probe succeeds it advertises both operations, names
`actionsSummaryPath` and `actionsControlPath` in the gateway configuration, and
creates the mode-0700 `actions-control` directory.

### What a step can see

Each step runs under `bwrap --unshare-all --clearenv`:

* no network namespace with a route out;
* a read-only `/usr` (and the host's `/bin`, `/lib*` as it has them);
* `/workspace`: a plain `git archive` export of the commit, not a clone, so
  nothing points back outside the sandbox;
* a scratch home, a private `/tmp`, and an environment holding only `PATH`,
  `HOME`, `LANG`, `CI`, `FORKMESH_ACTIONS`, `FORKMESH_REPO`, `FORKMESH_COMMIT`,
  `FORKMESH_REF`, `FORKMESH_RUN_ID`, `FORKMESH_EVENT`, `FORKMESH_NODE` and the workflow's own
  `env:` (which may not set `PATH`, `HOME` or `LD_*`).

It cannot see the node's state directory, its identity key, or the repositories
it serves. **A mirror holds no Actions variables**: it is somebody else's
machine, so a secret placed there would be readable by its operator.
`${{ vars.NAME }}` expands to nothing and the log says so. Workflows that need
secrets or network access (deploys, releases) belong on the owner's own
desktop node; pin them there with `runs-on:`.

### What runs, and when

* `on: push` — when a branch head moves. Without `on.push.branches` a workflow
  follows the **default branch only**: mirrors receive every agent and topic
  branch, and a small node cannot run CI for all of them. `on.push.paths` is
  honoured. The node's own `forkmesh/*` ledger branches never trigger runs,
  and the first sight of a repository only records where its branches are.
* `on: workflow_dispatch` — from the web's **Run workflow**.
* `runs-on:` is matched against the node's name, `linux` and `mirror`.
* `needs:` naming another workflow holds a run until that workflow's run for
  the same commit succeeds, and skips it if that run ends any other way.
* `pull_request` and `release` runs are not started on mirrors.
* One run at a time, an hour per run, 8 MiB of log, twenty queued runs, fifty
  runs kept. A newer push supersedes an older queued run of the same workflow
  on the same branch.

A workflow file outside the supported YAML subset is listed as invalid with
the line it stopped at, rather than run in part.

## Trust model

The gateway is the process that faces the internet, so it is treated as less
trusted than the executor that runs code. `actions-summary.json` is therefore
written by the executor and only *read* by the gateway. Control flows the
other way, so the gateway is a **courier, not an authority**: it
stores the Worker's router capability next to the exact request body, and the
executor verifies that Ed25519 signature itself against the router public key
before it acts. A compromised gateway account can drop or delay a control
request. It cannot author one, alter one, or replay one after the executor has
consumed it.

Authority for a control request is therefore: an account session that is the
repository owner, an owner of the owning node, or an organization writer
(checked and audited by the Worker) **and** the router key (checked by the
gateway and again by the executor).

## Operation `actions-control`

`POST /v1/repositories/{owner}/{repository}/actions-control`

* In `PUBLIC_OPERATIONS`; never in `DEFAULT_PUBLIC_OPERATIONS`, never in
  `DIRECT_PUBLIC_READ_OPERATIONS`, and not reachable through the mirror
  dashboard's `/api/repo` alias. Query allowlist is empty.
* Always requires the signed router capability (`_authorize`).
* The gateway configuration must list `actions-status` as well, and must name
  `actionsControlPath`: an absolute, normalized directory whose final
  component is `actions-control`, that is a real (non-symlink) directory owned
  by the gateway's effective UID/GID with mode `0700`. The packaged path is
  `/var/lib/forkmesh-mirror/gateway/actions-control`.
* Body: at most 4 KiB of UTF-8 JSON, exact keys per action (below).

### Request body (Worker → gateway)

Common keys, always present:

| Key | Value |
| --- | --- |
| `schemaVersion` | `1` |
| `type` | `"forkmesh.actions-control-v1"` |
| `requestId` | `^[A-Za-z0-9_-]{16,64}$`, chosen by the browser, idempotency key |
| `action` | one of the actions below |
| `actor` | account name that asked, `^[a-z0-9][a-z0-9._-]{0,63}$` |
| `issuedAt` | integer ms; must be within ±60 s of the gateway clock |

Action-specific keys (exactly these, nothing else):

| `action` | Extra keys |
| --- | --- |
| `dispatch` | `workflow` (path), `ref` |
| `rerun` | `runId` |
| `cancel` | `runId` |
| `approve` | `runId` |
| `reject` | `runId` |
| `set-workflow-enabled` | `workflow` (path), `enabled` (boolean) |
| `set-repository-enabled` | `enabled` (boolean) |

* `workflow`: `^\.forkmesh/[A-Za-z0-9._-]{1,96}\.ya?ml$`
* `ref`: `^refs/heads/[A-Za-z0-9._/-]{1,200}$`, no `..`, no `//`, no trailing
  `/` or `.lock`
* `runId`: integer `1 … 2^53-1`

### Gateway behaviour

1. Validate the body exactly (unknown or missing key → `GatewayError`).
2. Refuse when `actions-summary.json` cannot currently be loaded (executor
   offline), or is still a version 1 document (an executor that predates this
   contract): either way there is nobody to consume the request.
3. Write `{actionsControlPath}/{requestId}.json` atomically (temporary file in
   the same directory, `O_EXCL|O_NOFOLLOW`, mode `0640`, `fsync`, `rename`).
   The document is the **envelope**:

   ```json
   {
     "schemaVersion": 1,
     "type": "forkmesh.mirror-actions-control",
     "node": "<this gateway's node>",
     "owner": "<owner>",
     "repository": "<repository>",
     "receivedAt": 0,
     "capability": {
       "method": "POST",
       "path": "/v1/repositories/<owner>/<repository>/actions-control",
       "requestId": "<X-ForkMesh-Request-Id>",
       "issuedAt": 0,
       "bodySha256": "<X-ForkMesh-Body-Sha256>",
       "signature": "<X-ForkMesh-Signature>"
     },
     "body": "<the exact request body bytes, as a UTF-8 string>"
   }
   ```

4. Idempotency: if `{requestId}.json` already exists with a byte-identical
   `body`, answer accepted again without rewriting; with a different body,
   fail `control request id was already used`.
5. Bound: at most 32 pending `*.json` files in the directory; beyond that fail
   `control queue is full`. Files older than 10 minutes are removed first.
6. Success: HTTP `202`
   `{"ok":true,"status":"accepted","requestId":"…","node":"…"}` with
   `Cache-Control: no-store`.
7. Every refusal, including a full queue and a reused id, is the gateway's
   uniform `404 {"ok":false,"error":"not_found"}`; the Worker reports all of
   them as `actions_node_unavailable`. A replay is idempotent only for the
   same owner, repository and body.

### Executor behaviour

The daemon's runner has its own 4 s loop (the daemon's sync cycle can be half
an hour apart on a relay-fed node, far too slow to answer a button press). The
control directory is `actions-control` beside the gateway configuration. For
each regular, single-link, non-symlink `*.json` file of at most 16 KiB it:

1. Parses the envelope with exact keys; checks `node` equals its own mirror
   node and `path` equals the owner/repository route.
2. Recomputes `sha256(body)` and compares with `capability.bodySha256`.
3. Rebuilds the canonical capability message
   (`forkmesh-masked-proxy-v1\n<node>\nPOST\n<path>\n<bodySha256>\n<requestId>\n<issuedAt>`,
   identical to `edge_routing.request_message`) and verifies the Ed25519
   signature against the gateway configuration's `routerPublicKey`.
4. Rejects a capability older than 10 minutes, and any body `requestId` it has
   already recorded (a ring of the last 256 ids, saved in the runner's
   `actions/state.json` *before* anything is applied, so a crash can lose a
   request but can never run one twice).
5. Parses the body with the same exact-key rules as the gateway.
6. Deletes the file, applies the action, records the outcome.

An envelope that fails steps 1–5 is deleted and recorded as `rejected` only
when its body `requestId` could be read from a *verified* body; otherwise it is
deleted silently and logged locally.

Applying an action:

| Action | Effect | Rejection reasons |
| --- | --- | --- |
| `dispatch` | The manual-run path: resolve `ref` in the served mirror, read the workflow at that commit, require `on: workflow_dispatch`, create the run | `repository_not_found`, `actions_disabled`, `ref_not_found`, `workflow_not_found`, `workflow_invalid`, `not_manual`, `workflow_disabled`, `wrong_node` |
| `rerun` | A new run of the same workflow at the same commit | `run_not_found`, `actions_disabled`, `wrong_node` |
| `cancel` | Stop a running run, cancel a queued one | `run_not_found`, `not_cancellable` |
| `approve` / `reject` | Nothing on a mirror waits for approval: a push was authorized by whoever could move the branch, a dispatch by the owner's session, and pull-request runs are not started here | `run_not_found`, `not_awaiting_approval` |
| `set-workflow-enabled` | Add/remove the path in the repository's `disabledWorkflows` | `repository_not_found`, `workflow_not_found` |
| `set-repository-enabled` | Sets the repository's `webDisabled` flag in the runner's state. A paused repository queues no push runs and refuses dispatch and re-run. This can only narrow: whether the runner exists at all is `enableActions`, the operator's | `repository_not_found` |

A run always belongs to the envelope's `owner`/`repository`; a `runId` from
another repository is `run_not_found`.

## Summary v2 (`actions-summary.json`)

`schemaVersion` becomes `2`; `type` is unchanged. The gateway and the Worker
accept both `1` and `2`. Version 1 is exactly the old document.

The Go runner always writes version 2. It can, because the daemon only names
the Actions paths after checking that the gateway script it supervises reads
them (see *Enabling Actions on a mirror*).

Version 2 adds:

Per run (in addition to the v1 keys):

| Key | Value |
| --- | --- |
| `workflowPath` | workflow path pattern above, or `""` |
| `pullNumber` | integer ≥ 0 (0 = not a pull-request run) |
| `steps` | ≤ 64 of `{"name": ≤120 chars single line, "status": step status}` |

Step status: `pending | running | success | failed | skipped | cancelled`.

Top level:

| Key | Value |
| --- | --- |
| `repositories` | ≤ 64 of `{"owner","repository","actionsEnabled":bool,"workflows":[…]}` |
| `controls` | ≤ 32 newest-first of `{"owner","repository","requestId","action","status","reason","runId","finishedAt"}` |

Workflow entry (≤ 32 per repository):
`{"path","name"(≤160),"events":[≤8 of ≤40-char event names],"manual":bool,"enabled":bool,"runsOn":[≤8 labels of ≤64 chars],"valid":bool}`.

Control entry: `status` is `applied | rejected`; `reason` is `""` when
applied, otherwise one of the rejection reasons above or `invalid_request`;
`runId` is the created/affected run or `0`.

Nothing in v2 carries workflow content, commands, variables, or local paths.
The 256 KiB bound still applies; log tails are trimmed first, as in v1.

### Gateway response for one repository

v1 response keys are unchanged. For a v2 document the response adds
`schemaVersion: 2`, `actionsEnabled`, `workflows`, `controls`, and per-run
`workflowPath`, `pullNumber`, `steps` — filtered to the requested repository,
with `owner`/`repository` stripped from runs and control entries. If the
repository has no `repositories` entry, `actionsEnabled` is `false` and
`workflows` is `[]`.

## Worker routes

| Route | Method | Notes |
| --- | --- | --- |
| `/api/repo/{owner}/{repo}/actions/runs` | GET | Existing. Now passes v2 fields through after revalidating every one. Adds `controlAvailable: bool` — whether the answering node advertises `actions-control`. |
| `/api/repo/{owner}/{repo}/actions/control` | POST | New. Session required, same authorization and audit (`repository.actions_control`) as the read. Body keys: `requestId`, `action`, the action-specific keys, optional `sessionToken`. The Worker adds `schemaVersion`, `type`, `actor`, `issuedAt`. |

The control request goes to **the node named in the body's `node` key** — the
node whose summary the browser is looking at — never to "any candidate", since
run ids are per executor. `node` is a required browser→Worker key and is not
forwarded in the gateway body. Responses:

* `202 {"ok":true,"status":"accepted","requestId","node"}`
* `400 invalid_request`, `401 invalid_session`, `403 forbidden`,
  `404 not_found`, `405 method_not_allowed`, `413 request_too_large`
* `503 {"error":"actions_node_unavailable"}` with the expected-degraded
  header when the node is not a current attested candidate advertising
  `actions-control`, or did not accept.

The browser learns the outcome from the next `actions/runs` poll: the matching
`controls` entry, and the run list itself.
