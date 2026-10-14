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

Nodes receive queued ops in `/api/sync` (`branchOps`) or through the signed
`GET`. Queueing sends a `branches` account event to wake the node. Result
`status` is `applied`, `already` or `refused`; the relay keeps the newest
`MAX_BRANCH_OP_RESULTS` (20) per repository, encrypted. The queue holds up to
1024 ops per repository.

The Branches tab and compare page offer Merge (with a fresh `merge-preview`),
Update and Delete, then poll `GET /branches/ops` every 2 seconds for up to 3
minutes.
