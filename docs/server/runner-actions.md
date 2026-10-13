# Actions on a runner when the desktop is offline

Added 2026-10-08. Before this change, a repository's `.forkmesh/*.yml` push
workflows ran only on the owner's desktop, and only for commits the desktop
saw in its own clones. A merge on the web, a phone merge, or a push straight
to a core while the desktop was asleep ran nothing. The phone's Actions
screen stayed empty until the desktop came back.

Now, when the default branch of a repository on the core nodes moves and the
owner's desktop is not connected, the app queues an **Actions job**. A runner
(`role: "runner"`, `enableActions: true`) leases it, runs the matching
workflows, and reports the runs. Those runs appear in the existing Actions
"Runs" list on the web and on phones.

Code: `app/src/runner_actions.py` (queue, lease, report, snapshot),
`app/src/core_push.py` (`queue_runner_actions`), `server/runner_actions.go`
(executor), `server/runner.go` (claim loop), migration
`0248_repo_action_jobs.sql`.

## When a job is queued

`core_push.queue_runner_actions(env, owner, repo, commit, node, pusher, branch)`
queues a job only when all of these hold:

- The commit is a real object id. A deleted branch is not queued.
- `node`, the core that accepted the push or ran the merge, is listed in
  `RUNNER_POOL_TARGET_NODES`. The runner clones from a core, so a repository
  that only a desktop serves is never queued.
- An active runner lists the owner in its `allowed_owners`. This is the same
  opt-in that agent jobs use.
- No desktop is connected for the repository. The check is
  `_node_events_object_holds_node`, run for the owner and for the
  organization's linked `node_owner`. Core names are skipped, because a core's
  own socket does not mean a desktop is present. When the lookup fails, the
  desktop counts as offline.

There are three callers:

| Event | Where | Commit |
|---|---|---|
| A git push to a core moves the default branch | `core_push.record_accepted_push` → `_queue_pushed_actions` | the pushed tip |
| A pull request is merged on a core | `route_handlers` pull-merge handler | `result.baseAfter` |
| An agent branch is merged on a core | `agent_branches._publish_core_merge` | `result.baseAfter` |

The branch is the repository's default branch (`_default_branch`). A pull
request merged into another base still names that base's tip. The runner
checks that the commit is an ancestor of the default branch and runs nothing
when it is not.

Coalescing (`runner_actions.enqueue`):

- A commit that already has a job is not queued again.
- A newer push replaces the commit of a job still `queued` for that
  repository. Only the newest tip runs.
- At most one job per repository is `leased` or `running` at a time.

## Lease, run, report

The runner adds `actionsMax: 1` to `POST /api/runners/claim` when its Actions
slot is free and it has a free job slot. When no provider CLI is signed in, it
sends `providers: ["actions"]`. That name is not a provider, so both old and
new apps lease no agent jobs to it. An empty list would mean "every provider"
to an app deployed before this change.

`runner_actions.lease` hands out jobs of `kind: "actions"`. Each carries a
router-signed **read** grant (`edge_routing.runner_grant_message`, access
`read`), a clone URL on a core gateway, the branch, and the commit. An actions
job never carries a push grant. The core is the one that accepted the push
when it is a pool target with a gateway, and otherwise the first pool target
that has one. A claim counts actions jobs against its `max`.

The runner (`runnerActions.execute`) does the following:

1. Validates the lease. The read grant must name this runner, the owner and
   the repository. The clone URL must be the core's `/v1/runner/` route. The
   owner must pass the runner's own `allowedOwners`.
2. Refreshes its repository cache (`refreshCache`, shared with agent jobs).
   It resolves the default-branch tip, and checks that the leased commit is
   an ancestor of that tip.
3. Picks the workflows at that commit that run on `push`, match
   `on.push.branches` if given, and pass `runs-on` for the labels
   `<runner name>`, `linux` and `runner`. A workflow pinned to the desktop
   (`runs-on: forkmesh`, such as `deploy-nodes.yml` and
   `mac-native-build.yml`) is skipped. `on.push.paths` is not applied,
   because the job carries no "before" commit. That matches the mirror rule
   that an unknown diff counts as a match.
4. Runs the workflows one at a time, ordered by `needs:` (matched by name or
   path). A run whose dependency did not succeed is `skipped`.
5. Reports to `POST /api/runners/actions-report`
   (`{jobId, leaseId, status, commit, runs, message}`) once when the plan is
   known, at each workflow start, and at the end (`completed`, or `failed` for
   a refused or unfetchable job).

### What a step gets

- An exported copy of the commit (`git archive`, not a clone) in
  `<workRoot>/.actions/<job>-<n>/src`. The workspace is owned by the job
  user and removed after the run (`removeTreeUnder`).
- The runner's job sandbox (`jobSandbox.runner`): the `forkmesh-job` uid, a
  systemd scope with `MemoryMax`/`CPUQuota`/`TasksMax`, and the job user's
  nftables egress rules. Private and link-local networks are refused; the
  internet is reachable.
- A cleared environment (`env -i`). `HOME` and `TMPDIR` are the run's own
  scratch directories, never the job user's home, which holds the provider
  logins. `PATH`, `CI=true`, `FORKMESH_ACTIONS=1`, `FORKMESH_REPO`,
  `FORKMESH_COMMIT`, `FORKMESH_REF`, `FORKMESH_EVENT=push`, `FORKMESH_NODE`
  and the workflow's `env:` are set.
- No Actions variables. `${{ vars.X }}` expands to nothing and the log says
  so. A runner serves several owners, so it holds no secrets.
- One hour per workflow (`actionsRunTimeout`). The whole job ends at the
  lease timeout (90 minutes).

Bubblewrap is not used. The daemon unit's `RestrictNamespaces=true` is
inherited by its job scopes, so `bwrap` cannot create its namespaces there
(checked on fmnext-r1, 2026-10-08).

## Storage and display

Table `repo_action_jobs` (one row per queued push). `data` holds the sealed
`{runs, commit}` the runner last reported; each run keeps a 16 KiB log tail.
Run ids are `job id × 100 + n`.

The lifecycle (`runner_actions.sweep`, on every claim):

- A lease past `expires_at` is requeued once (`attempts`), then marked
  `failed`.
- A job nobody leases within a day becomes `expired`.
- Finished rows are deleted after 30 days.

`GET /api/repo/<owner>/<repo>/actions/runs` (`repository_actions_status_handler`)
first asks the mirrors, as before. When none answers, it serves
`runner_actions.snapshot`: the same v1 summary shape (`ok`, `node`,
`updatedAt`, `expiresAt`, `runs`), plus `source: "runner"` and
`controlAvailable: false`. Re-run and cancel are not offered for runner runs.

Changed 2026-10-10: when the repository has no job yet but an active runner
lists the owner, the snapshot is still served, with an empty `runs` list. The
Actions tab then shows "Runs on runner" and "No runner run yet" instead of the
old "No mirror is running Actions for this repository" screen, which told the
reader to configure a mirror even though the runner path was set up. When no
runner serves the owner either, the tab says "No runs reported for this
repository yet" and explains that desktop runs show in the desktop app and on
paired phones, not on the web.

## Configuration

- Runner: `"enableActions": true` in `mirror-node.json`. `deploy/core/render.py`
  sets it for every rendered runner. On a runner it no longer starts the
  mirror's local `actionsRunner`.
- App: `RUNNER_POOL_TARGET_NODES` (already set for agent jobs). No new
  setting.

## Traps

- **A runner rendered before 2026-10-08 never leases Actions jobs.** Its
  `/etc/forkmesh-next/mirror-node.overrides.json` has no `"enableActions":
  true`, so `server/runner.go` never builds `runnerActions` and queued jobs
  expire after a day. fmnext-r1 still had its 2026-09-27 overrides on
  2026-10-10. Re-render it from the operator desktop with
  `tools/next_fleet.py roll-runners` (dry run first, then with the execute
  flag).
- **Deploy nodes never runs on a runner.** `deploy-nodes.yml` is pinned to
  `runs-on: forkmesh` because it needs the fleet SSH key, which a runner must
  not hold. It runs only on the desktop, so a push while the desktop is away
  deploys nothing until the desktop comes back.

- **The desktop comes back after a runner already ran the commit.** The
  desktop does not know about runner runs, so it may run the same workflows
  again when it catches up on served-mirror heads. The two run lists are
  separate: the phone's "Workflows" tab and desktop runs still come from the
  desktop.
- **Desktop presence is the node-events socket.** A desktop that is open but
  has lost its socket counts as offline, so its push is run on a runner.
- **Hosted pushes with no tip.** A web-created repository's pin
  (`record_hosted_push_state`) carries only a refs digest. Those pushes queue
  through `record_accepted_push` when the core accepted them. The pin alone
  queues nothing, because it cannot tell an agent-branch push from a push to
  the default branch.
- **Workflows written for the desktop.** A push workflow with no `runs-on`
  now also runs on runners while the desktop is away. Pin one to the desktop
  with `runs-on: forkmesh` if it needs the desktop's tools or keys.

## Related docs

[actions-runner.md](actions-runner.md) (the mirror executor),
[../RUNNERS.md](../RUNNERS.md), [agent-jobs.md](agent-jobs.md).
