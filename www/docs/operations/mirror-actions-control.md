# Mirror Actions control and summary v2

The website's repository **Actions** tab reads run state from, and sends
control requests to, a mirror node running the Actions executor. This page is
the wire contract between:

| Component | File |
| --- | --- |
| Website | `app/public/dashboard/js/06-repo-actions.js` |
| Relay server | `app/src/repository_actions.py` |
| Mirror gateway | `tools/mirror_gateway.py` |
| Executor (Go mirror daemon) | `server/actions.go`, `server/actions_control.go`, `server/actions_workflow.go` |

The Go daemon (`forkmesh-mirror-node`) is the executor: it runs workflows,
writes the summary, and consumes control requests. Desktops that publish their
own runs still write a read-only version 1 summary. Nothing here can enable an
executor its operator turned off.

## Enabling Actions on a mirror

Actions run repository-supplied shell, so they are opt-in and fail closed:

1. Deploy a matching gateway and daemon (the daemon names Actions keys only
   when its gateway understands them).
2. Install `server/packaging/systemd/forkmesh-mirror-node-actions.conf` as a
   drop-in for `forkmesh-mirror-node.service`; it allows the namespaces
   `bubblewrap` needs.
3. Set `"enableActions": true` in `mirror-node.json` and restart.

At start the daemon probes a sandbox. On failure it logs `mirror actions are
unavailable: …`, runs nothing, and advertises neither `actions-status` nor
`actions-control`; it never runs a step unsandboxed.

Each step runs under `bwrap --unshare-all --clearenv` with no outbound network,
read-only system directories, a `git archive` export of the commit at
`/workspace`, a scratch home and `/tmp`, and only `PATH`, `HOME`, `LANG`, `CI`,
`FORKMESH_*` run variables, and the workflow's own `env:`. Steps cannot see node
state, identity keys, or other repositories. **A mirror holds no Actions
variables**, so workflows needing secrets or network belong on the owner's own
node (`runs-on:`).

Triggers: `on: push` when a branch head moves (default branch only unless
`on.push.branches` is set; `paths` honoured; ledger branches ignored) and
`on: workflow_dispatch`. `runs-on:` matches the node name, `linux`, and
`mirror`; `needs:` waits for another workflow's success on the same commit.
`pull_request` and `release` runs do not start on mirrors. Limits: one run at a
time, one hour per run, 8 MiB of log, twenty queued, fifty kept; newer pushes
supersede queued runs of the same workflow and branch.

## Trust model

The internet-facing gateway is less trusted than the executor. The executor
writes `actions-summary.json`; the gateway only reads it. For control, the
gateway is a **courier, not an authority**: it stores the router capability
beside the exact request body, and the executor verifies the Ed25519 signature
itself. A compromised gateway can drop or delay a request but cannot author,
alter, or replay one. Authorization requires a session that is the repository
owner, an owner of the node, or an organization writer (checked and audited by
the server) **and** the router key.

## Operation `actions-control`

`POST /v1/repositories/{owner}/{repository}/actions-control`

- Listed in `PUBLIC_OPERATIONS` only; always requires the signed router
  capability; no query parameters.
- Requires `actions-status` and an `actionsControlPath` that is an absolute,
  owner-only (`0700`), non-symlink directory named `actions-control`.
- Body: at most 4 KiB of JSON with exact keys.

| Key | Value |
| --- | --- |
| `schemaVersion` | `1` |
| `type` | `"forkmesh.actions-control-v1"` |
| `requestId` | `^[A-Za-z0-9_-]{16,64}$`, idempotency key |
| `action` | see below |
| `actor` | `^[a-z0-9][a-z0-9._-]{0,63}$` |
| `issuedAt` | integer ms, within ±60 s of the gateway clock |

| `action` | Extra keys |
| --- | --- |
| `dispatch` | `workflow`, `ref` |
| `rerun`, `cancel`, `approve`, `reject` | `runId` |
| `set-workflow-enabled` | `workflow`, `enabled` |
| `set-repository-enabled` | `enabled` |

`workflow` matches `^\.forkmesh/[A-Za-z0-9._-]{1,96}\.ya?ml$`; `ref` matches
`^refs/heads/[A-Za-z0-9._/-]{1,200}$` without `..`, `//`, or a trailing `/` or
`.lock`; `runId` is an integer `1 … 2^53-1`.

### Gateway behaviour

1. Validate the body exactly.
2. Refuse when the summary cannot be loaded or is still version 1.
3. Atomically write `{actionsControlPath}/{requestId}.json` (`O_EXCL|O_NOFOLLOW`,
   `0640`, fsync, rename) as an envelope of type
   `forkmesh.mirror-actions-control` holding `node`, `owner`, `repository`,
   `receivedAt`, the `capability` (method, path, request id, issued-at, body
   SHA-256, signature), and the exact `body` string.
4. A repeated `requestId` with an identical body is accepted again; a different
   body fails.
5. Keep at most 32 pending files; files older than 10 minutes are removed.
6. Success is `202 {"ok":true,"status":"accepted","requestId":"…","node":"…"}`
   with `Cache-Control: no-store`; every refusal is the uniform
   `404 {"ok":false,"error":"not_found"}`.

### Executor behaviour

A 4-second loop reads each regular, single-link `*.json` file (≤ 16 KiB) and:

1. checks exact keys, its own `node`, and the owner/repository path;
2. recomputes `sha256(body)` against `capability.bodySha256`;
3. verifies the signature over
   `forkmesh-masked-proxy-v1\n<node>\nPOST\n<path>\n<bodySha256>\n<requestId>\n<issuedAt>`
   with the configured `routerPublicKey`;
4. rejects capabilities older than 10 minutes and any `requestId` in its saved
   ring of the last 256 (persisted before applying, so a request never runs
   twice);
5. parses the body, deletes the file, applies the action, and records the
   outcome.

| Action | Effect | Rejection reasons |
| --- | --- | --- |
| `dispatch` | Run a workflow with `workflow_dispatch` at `ref` | `repository_not_found`, `actions_disabled`, `ref_not_found`, `workflow_not_found`, `workflow_invalid`, `not_manual`, `workflow_disabled`, `wrong_node` |
| `rerun` | New run of the same workflow and commit | `run_not_found`, `actions_disabled`, `wrong_node` |
| `cancel` | Stop or dequeue a run | `run_not_found`, `not_cancellable` |
| `approve` / `reject` | Nothing on a mirror awaits approval | `run_not_found`, `not_awaiting_approval` |
| `set-workflow-enabled` | Toggle the path in `disabledWorkflows` | `repository_not_found`, `workflow_not_found` |
| `set-repository-enabled` | Toggle `webDisabled` (can only narrow `enableActions`) | `repository_not_found` |

A `runId` from another repository is `run_not_found`.

## Summary v2 (`actions-summary.json`)

Readers accept `schemaVersion` 1 and 2; the Go runner writes 2. Version 2 adds:

- per run: `workflowPath`, `pullNumber` (0 if none), and up to 64 `steps` of
  `{"name","status"}` with status `pending | running | success | failed |
  skipped | cancelled`;
- `repositories` (≤ 64): `{"owner","repository","actionsEnabled","workflows"}`,
  each workflow (≤ 32) `{"path","name","events","manual","enabled","runsOn","valid"}`;
- `controls` (≤ 32, newest first): `{"owner","repository","requestId","action",
  "status","reason","runId","finishedAt"}` with status `applied | rejected`.

No workflow content, commands, variables, or local paths appear. The 256 KiB
bound applies, trimming log tails first. The gateway filters the response to
the requested repository; a repository without an entry reports
`actionsEnabled: false` and empty `workflows`.

## Server routes

| Route | Method | Notes |
| --- | --- | --- |
| `/api/repo/{owner}/{repo}/actions/runs` | GET | Revalidates and passes through v2 fields; adds `controlAvailable`. |
| `/api/repo/{owner}/{repo}/actions/control` | POST | Session required; audited as `repository.actions_control`. Browser sends `node`, `requestId`, `action`, action keys; the server adds `schemaVersion`, `type`, `actor`, `issuedAt`. |

The request goes only to the node named by `node` (run ids are per executor);
`node` is not forwarded in the gateway body. Responses: `202` accepted; `400`,
`401`, `403`, `404`, `405`, `413`; or `503 {"error":"actions_node_unavailable"}`
with the expected-degraded header. The browser learns the outcome from the next
`actions/runs` poll.
