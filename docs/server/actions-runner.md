# Mirror Actions runner

Opt-in runner for a repository's `.forkmesh/*.yml` workflows on a mirror.
Wire contract and operator steps:
[mirror-actions-control.md](../../www/docs/operations/mirror-actions-control.md).
Code: `server/actions.go`, `server/actions_workflow.go`,
`server/actions_control.go`; systemd drop-in
`server/packaging/systemd/forkmesh-mirror-node-actions.conf`.

## Rules

- Off unless `"enableActions": true`. Without a working bubblewrap sandbox,
  or with a gateway script that predates the Actions keys, nothing runs and
  the `actions-status` / `actions-control` operations are not advertised
  (`ensureGatewayActions`).
- Steps see a read-only `/usr`, an exported copy of the commit and a scratch
  home; no network, a cleared environment, and no view of the state
  directory, identity key or served repositories.
- Mirrors hold no Actions variables: `${{ vars.X }}` expands to nothing.
- `on: push` follows the default branch unless `on.push.branches` says
  otherwise; the node's own `forkmesh/*` ledger branches never trigger runs.
- One run at a time; `needs:`, `runs-on:`, `on.push.paths`, supersede, and a
  restart cancels what was in flight.
- A run's scratch tree (`<run>/work`) is removed before the run is marked
  finished; only its log is kept. Marking it first let a reader see a
  finished run whose tree was still there, which made
  `TestActionsPushRunsTheWorkflowInTheExportedCommit` fail about three times
  in four.
- Workflow YAML is read by a small subset parser (`actions_workflow.go`);
  `uses:` steps are dropped.

## Control requests

The gateway only couriers the Worker's signed capability and the request body
into `<mirror-gateway>/actions-control/`. `actions_control.go` verifies the
capability against the router public key, consumes each request once, and
records it before applying it, so a crash cannot apply it twice.

## Traps

- The web Actions tab polls `actions/runs` while runs are live
  (`app/public/dashboard/js/06-repo-actions.js`); that is by design, every read
  is a relay-to-mirror round trip, so the cadence backs off when idle.
- There is no desktop half of the executor; it lives in the daemon.
- On `role: "runner"`, `enableActions` does not start this executor. It
  takes Actions jobs from the app instead, for pushes that land on the
  cores while the owner's desktop is offline:
  [runner-actions.md](runner-actions.md).
