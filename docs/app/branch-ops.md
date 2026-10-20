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

Code: `repository_branch_delete_handler` in `app/src/route_handlers.py`,
`branch_delete_target` and `core_delete_branches` in
`app/src/agent_branches.py`. Tests: `app/tests/test_repo_branch_delete.py`,
the delete tests in `app/browser_tests/tests/dashboard-repo-branches.spec.js`,
`TestHostedPushMirrorPrunesADeletedBranch` in
`server/hosted_push_mirror_test.go`.

`POST /api/repo/<o>/<r>/branches/delete` with
`{"branches": [{"name": "feature", "expectedOid": "<tip>"}, ...]}` (a plain
name string is accepted too) deletes up to `MAX_BRANCH_DELETES` (100) branches
and answers `200` with one result per branch, in request order:

```json
{"ok": true, "defaultBranch": "main", "limit": 100,
 "counts": {"deleted": 1, "queued": 0, "pending": 0, "refused": 1, "failed": 0},
 "results": [{"branch": "feature", "status": "deleted", "error": "", "message": "Deleted feature."},
             {"branch": "main", "status": "refused", "error": "default_branch", "message": "..."}],
 "ops": []}
```

- **Who.** The repository owner, an organization owner or admin
  (`_authorize_issue_manager`), resolved from the account session in the
  `Authorization: Bearer` header. The session never travels in a cookie, so a
  cross-site form cannot send it. Anyone else gets `403 not_authorized` and
  nothing is read or changed. A read-only mirror answers `403
  read_only_mirror`.
- **Refused per branch** (`status: "refused"`): a name Git cannot use or
  ForkMesh's own `forkmesh/` refs (`invalid_branch`, `reserved_branch`), the
  default branch (`default_branch`), a protected branch (`branch_protected`,
  the same refs a push is refused for, see
  [branch-protection.md](branch-protection.md)), a branch the repository does
  not have (`branch_not_found`), and a branch whose tip is no longer the
  `expectedOid` the page showed (`stale_head`). The default branch and the
  tips come from the served repository, never from the request.
- **The whole request** is refused with `400` for an empty list or more than
  100 names, and with `503` (`retryable`, nothing deleted) when the branch
  list or the protection settings cannot be read.
- **How it deletes.** When cores serve the repository (every hosted
  repository, and node repositories with a core copy), each branch goes
  through the same signed path as the agent-branch and Branches-page deletes:
  a router-signed `forkmesh.branch-delete-v1` request to the gateway's
  `delete-agent-branch` operation, run by the
  [merge executor](../server/merge-executor.md). It pins the tip, refuses the
  default branch again, and in one ref transaction moves the tip to
  `refs/forkmesh/trash/branch/<name>/...` and deletes the branch, so the
  commits stay recoverable. The other cores get the same request
  (`_fan_out_cleanup`). A core that is still working after three tries
  reports `pending`. The relay never edits refs on disk, and `git push`
  deletes stay refused (`receive.denyDeletes`).
- **Without a core** a node-owned repository gets the deletes queued for its
  node like any other branch operation (`status: "queued"`, `expectedOid`
  required). A hosted repository has no node to queue for, so it answers
  `503 branch_delete_unavailable` instead.
- **Afterwards.** One `repository.branch_delete` audit event per branch
  (`success`, `denied`, `failed` or `requested`, with the branch, status and
  error). If anything was deleted, `record_hosted_push_state` notes the push
  for a hosted repository, which moves `pushed_at` and wakes every core: the
  primary's push mirror then sees the changed refs digest and pushes, and with
  prune the target drops the branch. A node-owned repository also gets a
  `source: "cores"` delete queued for its node, so the node's checkout does
  not publish the branch again.

The Branches page shows the controls only to a viewer that gate accepts:

- a trash button per branch that asks inline in the row ("Delete
  feature/x?", plus the number of unmerged commits) instead of a browser
  dialog;
- a checkbox per branch and a selection bar ("2 selected", Delete selected,
  Clear selection); Delete selected first shows a summary with the names and
  how many have commits not merged into the default branch;
- a "Merged into main" filter that lists only branches with nothing of their
  own (`ahead == 0`) and selects them, which replaced the old "Delete N
  merged" button and its `window.confirm`.

The page sends at most 20 branches per request, shows progress, and lists
each branch's result under the selection bar; a branch that was kept stays
selected and is marked "not deleted" in its row. Deleted branches disappear
after the page reloads the branch list.
