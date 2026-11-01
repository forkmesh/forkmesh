# Actions on mirrors and runners

The daemon has two executors for a repository's `.forkmesh/*.yml` workflows:
an opt-in executor on an independent mirror, and the runner-tier executor
that runs push workflows when the owner's desktop is offline. The runner
executor also serves instance Actions (every event, real clones, secrets,
push back, full logs); [instance-actions.md](instance-actions.md) is the
contract for that and overrides the runner details below where they differ.
Part of [the Go daemon docs](README.md).

## On a mirror

Wire contract and operator steps:
[mirror Actions control](../operations/mirror-actions-control.md).
Code: `server/actions.go`, `server/actions_workflow.go`,
`server/actions_control.go`; systemd drop-in
`server/packaging/systemd/forkmesh-mirror-node-actions.conf`.

### Rules

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
- Contributor branches (`refs/heads/pr/<account>/...`, pushable by any
  verified account when contributor pushes are on) never trigger runs either:
  their workflow comes from the pushed commit, and the sandbox has no process,
  memory or disk cap, so one push could fork-bomb or fill the node. The app's
  instance runner treats them as untrusted (`untrusted_branch`)
  (`TestActionsContributorBranchPushRunsNothing`).
- One run at a time; `needs:`, `runs-on:`, `on.push.paths`, supersede, and a
  restart cancels what was in flight.
- A run's scratch tree (`<run>/work`) is removed before the run is marked
  finished, so no reader sees a finished run whose tree is still there; only
  its log is kept (`TestActionsPushRunsTheWorkflowInTheExportedCommit`).
- Workflow YAML is read by a small subset parser (`actions_workflow.go`);
  `uses:` steps are dropped.

### Control requests

The gateway only couriers the App's signed capability and the request body
into `<mirror-gateway>/actions-control/`. `actions_control.go` verifies the
capability against the router public key, consumes each request once, and
records it before applying it, so a crash cannot apply it twice.

### Traps

- The web Actions tab polls `actions/runs` while runs are live
  (`app/public/dashboard/js/06-repo-actions.js`). Every read is an
  App-to-mirror round trip, so the cadence backs off when idle.
- There is no desktop half of the executor; it lives in the daemon.
- On `role: "runner"`, `enableActions` does not start this executor. It
  takes Actions jobs from the app instead (next section).

## On a runner when the desktop is offline

A repository's `.forkmesh/*.yml` push workflows normally run on the owner's
desktop. When the default branch of a repository on the core nodes moves and
the owner's desktop is not connected, the app queues an **Actions job**; a
runner (`role: "runner"`, `enableActions: true`) leases it, runs the matching
workflows and reports the runs, which appear in the Actions "Runs" list on the
web and phones.

Code: `app/src/runner_actions.py` (queue, lease, report, snapshot),
`app/src/core_push.py` (`queue_runner_actions`), `server/runner_actions.go`
(executor), `server/runner.go` (claim loop), migration
`0248_repo_action_jobs.sql`.

### When a job is queued

`core_push.queue_runner_actions(env, owner, repo, commit, node, pusher, branch)`
queues only when the commit is a real object id, `node` is in
`RUNNER_POOL_TARGET_NODES`, an active runner lists the owner in
`allowed_owners`, and no desktop is connected for the repository
(`_node_events_object_holds_node` for the owner and the organization's
`node_owner`; core names skipped; a failed lookup counts as offline).

Callers: a push to a core that moves the default branch
(`core_push.record_accepted_push`), a pull request merge on a core, and an
agent branch merge on a core (`result.baseAfter`). The runner runs nothing
when the commit is not an ancestor of the default branch.

Coalescing (`runner_actions.enqueue`): one job per commit; a newer push
replaces the commit of a still-`queued` job; at most one `leased` or `running`
job per repository.

### Lease, run, report

The runner adds `actionsMax: 1` to `POST /api/runners/claim` when its Actions
slot is free; with no provider CLI signed in it sends `providers: ["actions"]`
so no agent jobs are leased to it. `runner_actions.lease` hands out
`kind: "actions"` jobs with a router-signed **read** grant, a clone URL on a
core gateway, the branch and the commit; never a push grant.

The runner (`runnerActions.execute`):

1. validates the grant (this runner, owner, repository), the `/v1/runner/`
   clone URL and its own `allowedOwners`;
2. refreshes its repository cache and checks the commit is an ancestor of the
   default-branch tip;
3. picks workflows that run on `push`, match `on.push.branches`, and pass
   `runs-on` for `<runner name>`, `linux` or `runner` (`runs-on: forkmesh`
   pins a workflow to the desktop); `on.push.paths` is not applied;
4. runs them one at a time in `needs:` order (a run whose dependency failed is
   `skipped`);
5. reports to `POST /api/runners/actions-report`
   (`{jobId, leaseId, status, commit, runs, message}`) when planned, at each
   workflow start, and at the end.

Each step gets a clone of the commit (exported with `git archive` before
instance Actions) under
`<workRoot>/.actions/<job>-<n>/src` (removed with `removeTreeUnder`), the
runner's job sandbox (`forkmesh-job` uid, systemd scope limits, egress rules
refusing private networks), a cleared environment with its own `HOME` and
`TMPDIR` plus `CI=true`, `FORKMESH_ACTIONS=1`, `FORKMESH_REPO`,
`FORKMESH_COMMIT`, `FORKMESH_REF`, `FORKMESH_EVENT=push`, `FORKMESH_NODE` and
the workflow's `env:`. No Actions variables (`${{ vars.X }}` is empty): a
runner serves several owners and holds no secrets. One hour per workflow;
the job ends at the 90-minute lease timeout. Bubblewrap is not used because
the daemon's `RestrictNamespaces=true` applies to its job scopes.

### Storage and display

`repo_action_jobs` holds one row per queued push; `data` keeps the sealed
`{runs, commit}` with a 16 KiB log tail per run. Run ids are
`job id × 100 + n`. `runner_actions.sweep` (on every claim) requeues an
expired lease once then fails it, expires jobs nobody leased within a day, and
deletes finished rows after 30 days.

`GET /api/repo/<owner>/<repo>/actions/runs` asks the mirrors first; when none
answers it serves `runner_actions.snapshot` (v1 summary shape plus
`source: "runner"`, `controlAvailable: false`), with an empty `runs` list when
a runner serves the owner but has no job yet. Re-run and cancel are not
offered for runner runs.

### Configuration

- Runner: `"enableActions": true` in `mirror-node.json` (set by
  `deploy/core/render.py` for every rendered runner).
- App: `RUNNER_POOL_TARGET_NODES`.

### Traps

- A runner whose overrides lack `"enableActions": true` never leases Actions
  jobs; re-render it with `tools/next_fleet.py roll-runners`.
- Workflows that need fleet keys (`deploy-nodes.yml`) are pinned to the
  desktop with `runs-on: forkmesh`; a push while the desktop is away deploys
  nothing until it returns. Pin any workflow that needs the desktop's tools.
- The desktop may run the same workflows again when it catches up; the two run
  lists are separate.
- A desktop that is open but lost its node-events socket counts as offline.
- A hosted repository's pin alone queues nothing; only accepted pushes do.

### Instances

With `ACTIONS_INSTANCE_RUNNER` set (a self-hosted instance), every pushed
branch queues a job whether or not a desktop is online, jobs carry other
events, secrets, a push grant and a job token, and runs can be dispatched,
cancelled and re-run from the web: [instance-actions.md](instance-actions.md).
Coalescing is per branch; the one-active-job rule is per workflow (push jobs
share one). The Actions report body may be up to 8 MiB and its answer
carries `cancel`.

Runner tier: [runners](../operations/runners.md); agent jobs on the same
runners: [agent-jobs.md](agent-jobs.md).
