# Branch management and merging from the website

Part of [the App docs](README.md). Code: `app/src/route_handlers.py`
(`BRANCH_OPS`, `repository_branch_ops_handler`), tables `branch_op_inbox` and
`branch_op_results`, `app/public/dashboard/js/07-repo-compose-branch.js`, and
on the desktop `desktop/src/WebBranchOps.cpp`. Tests:
`app/tests/test_repo_branch_management.py`,
`desktop/tests/test_web_branch_ops.cpp`,
`app/browser_tests/tests/dashboard-repo-branches.spec.js`.

The relay has no Git. A single `merge`, `update` or `delete` runs immediately
on a core when the cores serve the repository (`_branch_op_on_cores`, via the
[merge executor](../server/merge-executor.md)); the client repeats the same
body every 2 s while it answers `202 processing`. Otherwise the operation is
queued for the node that owns the checkout, which applies it with local Git
and reports back.

| `op` | Fields | Effect |
| --- | --- | --- |
| `delete` | `branch`, `expectedOid` | Delete if the tip is unchanged |
| `update` | `branch`, `expectedOid`, `base` | Merge `base` into `branch` without a checkout |
| `create` | `branch`, `startOid` | Create at `startOid` |
| `merge` | `branch`, `expectedOid`, `base`, `baseOid`, `deleteBranch` | `--no-ff` merge into `base` (`Merge <branch> into <base>`), optional clean-up |

Invariants: both tips are pinned; a merge is refused if either moved, on
conflict, while an agent runs on the branch, or from the default branch. A
checked-out `base` moves only with `git merge --ff-only` (local edits are
never overwritten); clean-up never forces a worktree removal and deletes refs
with an exact-tip `update-ref -d`.

## Routes

| Method and path | Caller | Auth |
| --- | --- | --- |
| `GET/POST /api/repo/<o>/<r>/branches/ops`, `DELETE ...?id=` | page | manager session (`_authorize_issue_manager`) |
| `GET /api/repo/<o>/<r>/branches/ops?owner=&ts=&sig=` | owning node | node-signed; drain-on-read |
| `POST /api/repo/<o>/<r>/branches/ops/results` | owning node | node-signed; `{results: [...]}` |
| `DELETE /api/repo/<o>/<r>/branches/ops/results` | page | manager session |
| `POST /api/repo/<o>/<r>/branches/delete` | page | manager session; see below |

Nodes receive queued ops in `/api/sync` (`branchOps`) or through the signed
`GET`. Queueing sends a `branches` account event to wake the node. Result
`status` is `applied`, `already` or `refused`; the relay keeps the newest
`MAX_BRANCH_OP_RESULTS` (20) per repository, encrypted. The queue holds up to
1024 ops per repository.

The Branches tab and compare page offer Merge (with a fresh `merge-preview`),
Update and Delete, then poll `GET /branches/ops` every 2 seconds for up to 3
minutes.

## Deleting branches

Code: `repository_branch_delete_handler` and the `_branch_delete_*` job
helpers in `app/src/route_handlers.py`, `git_branch_name`,
`branch_delete_target`, `branch_delete_job_step` and
`branch_delete_job_publish` in `app/src/agent_branches.py`, table
`branch_delete_jobs` (migration `0275_branch_delete_jobs`). Tests:
`app/tests/test_repo_branch_delete.py`, the delete tests in
`app/tests/test_agent_branch_gateway.py` and
`app/browser_tests/tests/dashboard-repo-branches.spec.js`,
`TestBranchDeleteAcceptsEveryNameGitAllows` and
`TestBranchDeleteRemovesABranchWithAPlusInItsName` in
`server/merge_branch_test.go`, `TestHostedPushMirrorPrunesADeletedBranch` in
`server/hosted_push_mirror_test.go`.

A deletion is a server-side job that deletes one branch at a time.

- `POST /api/repo/<o>/<r>/branches/delete` with
  `{"branches": [{"name": "feature", "expectedOid": "<tip>"}, ...]}` (a plain
  name string is accepted too), at most `MAX_BRANCH_DELETES` (200). It checks
  every name, stores the job and answers at once: `202` with the job while
  branches are waiting, `200` when nothing needs a core (every name refused,
  or a node-owned repository whose deletes were queued for its node).
- `GET /api/repo/<o>/<r>/branches/delete?job=<id>` (or without `job`, the
  newest job) answers the job: `state` (`running` or `done`), `total`,
  `done`, `current`, `counts`, and one result per branch in request order.
- While a job runs, a second `POST` answers `409 delete_running` with that
  job, so two jobs never run for the same repository.

```json
{"ok": true, "job": {"id": 7, "state": "running", "total": 27, "done": 12,
  "current": "feature/x", "defaultBranch": "main", "limit": 200,
  "counts": {"deleted": 11, "refused": 1, "waiting": 15, "queued": 0, "pending": 0, "failed": 0},
  "results": [{"branch": "feature/a", "status": "deleted", "error": "", "message": "Deleted feature/a."}, "..."]},
 "ops": []}
```

- **Who.** The repository owner, an organization owner or admin
  (`_authorize_issue_manager`), for both `POST` and `GET`, resolved from the
  account session in the `Authorization: Bearer` header. The session never
  travels in a cookie, so a cross-site form cannot send it. Anyone else gets
  `403 not_authorized`. A read-only mirror answers `403 read_only_mirror`.
- **Names.** Any name `git check-ref-format --branch` accepts is a branch
  name: `+`, `#`, `,`, `@` and non-ASCII letters are allowed; spaces, control
  characters, `~ ^ : ? * [ \`, `..`, `//`, `@{`, a leading `-` or `/`, a
  trailing `/` or `.`, a component starting with `.` or ending in `.lock`,
  `HEAD` and `@` are not, nor more than 255 bytes. The app
  (`git_branch_name`), the gateway (`_deletable_branch`) and the merge
  executor (`validDeletableBranch`) apply the same rules for a delete. Before,
  a narrow pattern refused real branches such as `feature/research+`.
- **Refused per branch** (`status: "refused"`): an unusable name
  (`invalid_branch`), ForkMesh's own `forkmesh/` refs (`reserved_branch`), the
  default branch (`default_branch`), a protected branch (`branch_protected`,
  see [branch-protection.md](branch-protection.md)), a branch the repository
  does not have (`branch_not_found`), and a tip that is no longer the
  `expectedOid` the page showed (`stale_head`). The default branch and the
  tips come from one read of the served repository when the job is created,
  never from the request. Refusals are audited when the job is created.
- **The whole request** is refused with `400` for an empty list or more than
  200 names, and with `503` (`retryable`, nothing stored) when the branch list
  or the protection settings cannot be read.

### How the job runs

On 2026-10-20 an owner deleted 44 branches of a large repository on a 1-CPU,
1 GB instance. The page sent 20 per request; the app gave each branch three
quick tries and moved on while the core was still working, so up to 20
executor jobs ran at once, each followed by a gateway reload and a full cache
warmup of every repository. The load reached 8.5, the request took 71 s,
later requests failed and 21 branches were kept. The job design fixes that:

- **One worker, one branch at a time.** The `POST` hands the job to a single
  background worker per app process (`HOST_BACKGROUND`, key
  `branch-delete-worker`), which drains every running job in id order. A job
  is claimed with a 60 s lease (`lease_until`), so two app processes never
  work on the same job, and a job whose worker died is picked up again by the
  next status poll. Without a background runner the status poll advances the
  job one step per request.
- **Each branch is finished before the next starts.** The worker sends the
  router-signed `forkmesh.branch-delete-v1` request (`discard`, pinned tip) to
  one core and polls the same request id until it is terminal, waiting 1, 2,
  3, 5, then 8 s between polls while the core answers `202 processing`. A core
  that does not answer is retried with the same backoff up to 6 times before
  the branch is reported `failed` (`merge_node_unavailable`) and kept; a core
  still working after 120 polls reports it `pending`. Then the same delete
  goes to each other core in turn (`fanout`), again one at a time. The
  executor moves the tip to `refs/forkmesh/trash/branch/<name>/...` and
  deletes the branch in one ref transaction, so the commits stay recoverable.
  The worker sleeps 0.25 s between steps, and every core call is an `await`,
  so page requests on the same process keep being served.
- **The core side stays light.** On the gateway the delete job thread runs at
  a lower CPU priority (`_lower_thread_priority`, nice +10, inherited by the
  executor and its Git processes), and the cache warmup after a merge or
  delete is coalesced: it waits until no merge or delete finished for 5 s
  (`PREWARM_AFTER_MERGE_QUIET_SECONDS`) and then runs once, also at low
  priority, instead of once per branch.
- **Once at the end**, when every branch has a result: one
  `record_hosted_push_state` (moves `pushed_at` and wakes the cores, so a push
  mirror with prune drops the deleted branches on its next push), one
  `record_hosted_branch_count` (the catalog's `branchCount` for a hosted
  repository, which only an import used to set, so the repository header kept
  the old count), one `branches` wake, and for a node-owned repository a
  `source: "cores"` delete queued for its node. Each branch result is audited
  as `repository.branch_delete` when it is final.

Without a core, a node-owned repository gets its deletes queued for its node
(`status: "queued"`, `expectedOid` required) and the job is done at once. A
hosted repository has no node to queue for and answers
`503 branch_delete_unavailable`. `git push` deletes stay refused
(`receive.denyDeletes`).

### The page

The Branches page shows the controls only to a viewer the same gate accepts:
a trash button per branch with an inline confirm, a checkbox per branch with
a "Delete selected" bar and a confirm summary, and a "Merged into main"
filter that lists and selects the branches with nothing of their own.

Deleting sends one request for every selected branch, then polls the job every
1.5 s and shows "Deleted 12 of 27 · now feature/x". Each branch leaves the
list as soon as the job reports it deleted. Opening the Branches page while a
job runs picks it up again (`resumeRepoBranchDeleteJob`).

The list cannot go stale any more. After the 44-branch deletion the page kept
showing 45 branches while the API listed 22, because a deletion only updated
the page from a re-read that could be answered by a direct mirror or the
page's seeded reads, and the repository header's count came from the catalog
`branchCount` (also stored in `localStorage`, `forkmesh.repositoryCatalog.v1`)
that nothing updated. Now the page removes deleted branches from its
in-memory list at once, re-reads the list from the app itself
(`fetchRepoJson(..., { canonical: true })`: no direct mirror, no seeded read,
`cache: "no-store"`), and writes the new count into the repository object,
the catalog list in memory and the stored catalog; the server updates the
catalog record too.
