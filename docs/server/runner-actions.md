# Actions on a runner when the desktop is offline

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

## When a job is queued

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

## Lease, run, report

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

Each step gets an exported copy of the commit (`git archive`) under
`<workRoot>/.actions/<job>-<n>/src` (removed with `removeTreeUnder`), the
runner's job sandbox (`forkmesh-job` uid, systemd scope limits, egress rules
refusing private networks), a cleared environment with its own `HOME` and
`TMPDIR` plus `CI=true`, `FORKMESH_ACTIONS=1`, `FORKMESH_REPO`,
`FORKMESH_COMMIT`, `FORKMESH_REF`, `FORKMESH_EVENT=push`, `FORKMESH_NODE` and
the workflow's `env:`. No Actions variables (`${{ vars.X }}` is empty): a
runner serves several owners and holds no secrets. One hour per workflow;
the job ends at the 90-minute lease timeout. Bubblewrap is not used because
the daemon's `RestrictNamespaces=true` applies to its job scopes.

## Storage and display

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

## Configuration

- Runner: `"enableActions": true` in `mirror-node.json` (set by
  `deploy/core/render.py` for every rendered runner).
- App: `RUNNER_POOL_TARGET_NODES`.

## Traps

- A runner whose overrides lack `"enableActions": true` never leases Actions
  jobs; re-render it with `tools/next_fleet.py roll-runners`.
- Workflows that need fleet keys (`deploy-nodes.yml`) are pinned to the
  desktop with `runs-on: forkmesh`; a push while the desktop is away deploys
  nothing until it returns. Pin any workflow that needs the desktop's tools.
- The desktop may run the same workflows again when it catches up; the two run
  lists are separate.
- A desktop that is open but lost its node-events socket counts as offline.
- A hosted repository's pin alone queues nothing; only accepted pushes do.

## Related docs

[actions-runner.md](actions-runner.md), [../RUNNERS.md](../RUNNERS.md),
[agent-jobs.md](agent-jobs.md).
