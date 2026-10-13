# Branch management and merging from the website

Part of [the App docs](README.md). Code: `app/src/route_handlers.py`
(`BRANCH_OPS`, `repository_branch_ops_handler`,
`repository_branch_op_results_handler`), tables `branch_op_inbox`
(migration 0176) and `branch_op_results` (migration 0219), the dashboard
fragment `app/public/dashboard/js/07-repo-compose-branch.js`, and on the
desktop `desktop/src/WebBranchOps.cpp` and `MainWindow::applyWebBranchOps`
(`desktop/src/MainWindowBranches.cpp`). Tests:
`app/tests/test_repo_branch_management.py`,
`desktop/tests/test_web_branch_ops.cpp`,
`app/browser_tests/tests/dashboard-repo-branches.spec.js`.

The relay has no Git. Every branch change typed on the website is queued for
the node that owns the repository's checkout (the one every mirror is
published from). That node applies the change with local Git, propagates it
to the mirrors, and reports what happened.

## Run on the cores first (2026-10-04)

A single `merge`, `update` or `delete` (not a batch, and not a body with
`"queue": true`) now runs at once on a core when the cores serve the
repository (`_branch_op_on_cores` → `agent_branches.core_branch_op`, through
the owner merge executor, [../server/merge-executor.md](../server/merge-executor.md)).
The owner's desktop does not need to be online. Otherwise (a private
repository, no core with the executor, a core that predates any-branch
support, or no answer to the first request) the operation is queued for the
desktop exactly as below.

- `merge` merges `branch` into `base` with both tips pinned
  (`expectedOid`, `baseOid`); `deleteBranch` removes the branch in the same
  transaction and then on every other core.
- `update` merges `base` (or the default branch) into `branch`, pinned to the
  branch tip the page saw and the base tip the App read.
- `delete` discards the branch at the pinned tip on every core.
- `202 {"executed": true, "status": "processing", "requestId", "node",
  "pinBase", "pinOid"?}`: repeat the same body with those four fields about
  every 2 s. The pins make every repeat the same executor request.
- `200 {"executed": true, "status": "merged" | "updated" | "deleted" |
  "already", "mergeOid", "branchDeleted", "cleanupPending", "message",
  "results"}`; `409` for `merge_conflict` (with `conflicts`), `stale_base`,
  `stale_head`, `unsupported_branch` (the default branch); `503` to retry.
- Every outcome is written to `branch_op_results` like a desktop report, and
  is audited with `"cores": true`.
- When a core removed the branch, a pinned `delete` is also queued for the
  owner's desktop (`"source": "cores"`), so its own copy goes too and cannot
  be pushed back. A desktop branch that moved since is refused there and
  kept.

## Operations

| `op` | Fields | What the node does |
| --- | --- | --- |
| `delete` | `branch`, `expectedOid` | Deletes the branch if its tip is still `expectedOid` |
| `update` | `branch`, `expectedOid`, `base` | Merges `base` (default branch when empty) into `branch`: fast-forward, or a `merge-tree` merge recorded without a checkout |
| `create` | `branch`, `startOid` | Creates the branch at `startOid` |
| `merge` | `branch`, `expectedOid`, `base`, `baseOid`, `deleteBranch` | Merges `branch` into `base` (default branch when empty); with `deleteBranch`, then removes the branch and its linked worktree |

`merge` is the web counterpart of the desktop's "Merge into main" and "Merge
& clean up" (`MainWindow::mergeWorktreeIntoMain`):

- It always records a merge commit (`--no-ff`), with `base` as the first
  parent and the message `Merge <branch> into <base>`, the same message the
  desktop writes.
- Both tips are pinned. The node refuses when either branch moved since the
  page read it, when the merge conflicts, when an agent is still running on
  the branch, or when `branch` is the default branch.
- When `base` is checked out (usually the main checkout's default branch),
  the node moves it with `git merge --ff-only <merge commit>` in that working
  tree, with hooks disabled. Git itself refuses if local changes would be
  overwritten, so unrelated local edits survive and nothing is clobbered.
  When `base` is not checked out, the node moves the ref with an exact-tip
  `update-ref`.
- After the merge, before any clean-up, the desktop runs the bookkeeping its
  own merge runs: `PullStore::reconcileMergedPulls`, the merge note for the
  celebration page, closing issues linked through the branch's agent
  sessions, and marking those sessions merged.
- Clean-up (`branchops::cleanUpMergedBranch`) runs in a second pass, so the
  pull reconciliation still sees the branch. It removes a linked worktree
  with `git worktree remove` (never `--force`; a worktree with local changes
  is kept and the report says so), then deletes the branch with an exact-tip
  `update-ref -d`. A branch checked out in the main checkout is kept.
- A branch already contained in `base` reports `already` and is still
  cleaned up when asked.

On the compare page, merging the default branch into another branch is an
`update` of that branch, since a `merge` whose source is the default branch
is refused.

## Routes

| Method and path | Caller | Auth |
| --- | --- | --- |
| `GET /api/repo/<o>/<r>/branches/ops` | page | repository owner or org owner/admin session (`_authorize_issue_manager`); returns `{ops, results}` |
| `GET /api/repo/<o>/<r>/branches/ops?owner=&ts=&sig=` | owning node | node-signed (`_authorized_repo_inbox_signing_key`); returns `{ops}` and deletes them (drain-on-read) |
| `POST /api/repo/<o>/<r>/branches/ops` | page | same; queues one operation |
| `DELETE /api/repo/<o>/<r>/branches/ops?id=` | page | same; cancels one queued operation |
| `POST /api/repo/<o>/<r>/branches/ops/results` | owning node | node-signed `ts`/`sig` query (`_authorized_repo_inbox_signing_key`, the drain token `/api/sync` uses); body `{results: [...]}` |
| `DELETE /api/repo/<o>/<r>/branches/ops/results` | page | manager session; clears the reported results |

The queue reaches the node in its `/api/sync` response (`branchOps`,
drain-on-read, each item carrying its inbox `id`), or, for a repository that
response does not list, from the signed `GET /branches/ops` above
(`MainWindow::drainWebBranchOpsFor`, run after every relay sync next to the
per-repo issue, pull and discussion drains).

Queueing wakes the node at once: `_branch_ops_wake` sends a `branches` event
to the repository owner's event socket and, when the page reached the
repository through an organization alias, to every owner and admin of that
organization. The desktop answers the event with a relay sync two seconds
later. The node reports each
outcome as `{requestId, op, branch, base, status, message, mergeOid,
branchDeleted}`, where `status` is `applied`, `already` or `refused`. The
relay keeps the newest `MAX_BRANCH_OP_RESULTS` (20) per repository,
encrypted like the inbox rows.

## The page

- Branches tab: "Merge into <default>" on every branch that is ahead of the
  default branch, next to "Update from <default>". It opens a dialog that runs
  a fresh `merge-preview` on the mirror and shows whether the merge is clean,
  the conflicting files, and the merge commit message. It also offers
  "Delete <branch> after merging", checked by default like the desktop's
  primary "Merge & clean up". Merging a branch that conflicts is disabled.
- Branches tab, "Delete": asks for confirmation on a desktop-sized screen. On
  a phone (`max-width: 767px` or a coarse pointer) it queues the delete
  straight away: the op still carries the tip the page saw, so the node
  refuses it if the branch moved, and the commits stay in the reflog.
  "Delete N merged" always asks, since it removes several branches at once.
- Compare page (`/<o>/<r>/compare/<base>...<head>`): a merge panel for
  managers with the same preview, "Merge into <base>", and "Update <head>
  from <base>" when the head is behind. The target may be any branch, not
  only the default one.
- After queueing, the page polls `GET /branches/ops` every 2 seconds, for up
  to 3 minutes, until the queue drains and a result arrives for each queued
  request. It then re-reads the branch listing and shows the node's report
  under "Reported by this repository's node".

## Traps

- The node applies only for a source-of-truth repository
  (`isSourceOfTruthRepo`). A request for a repository whose owner's desktop
  is offline waits in the queue (up to 1024 per repository).
- An organization repository (`forkmesh/forkmesh`) is stored under its backing
  node (`fmnext-n1` since the Next cutover), not under the desktop's account.
  `/api/sync` lists only the signing account's own repositories, so before
  2026-09-30 its branch operations were never drained: the per-repo fallback
  covered issues, pulls and discussions but not branch operations, and the
  `branches` event went to `fmnext-n1`'s socket, which no desktop holds. 66
  requests from the website sat in the queue.
- The Playwright spec polls on the real 2 second interval.
- The dialog uses only utility classes already present in the committed
  `app/public/dashboard/tailwind.css`. A new class there needs that file
  rebuilt.
