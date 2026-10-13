# Mirror merge executor

`forkmesh-mirror-node --config … --execute-mirror-merge` performs
owner-authorized web merges and pull-branch updates on a mirror, so an owner
can merge while the source desktop is offline. Code: `server/merge.go`,
`server/checks.go`, `server/pull_metadata_refs.go`. Operator view:
[mirror-node.md, "Owner-authorized web merges"](../../www/docs/operations/mirror-node.md#owner-authorized-web-merges).

## Flow

1. The Worker authenticates an organization owner, signs the exact
   request (expected base, head and ledger object ids), and sends it to one
   healthy mirror's gateway (`merge-preview`, `merge-pull`,
   `update-pull-branch`).
2. The gateway (`tools/mirror_gateway.py`) runs this binary with one JSON
   object on stdin: action `status`, `execute` or `register`. Request types:
   `forkmesh.pull-merge-executor-v1`, `forkmesh.pull-update-executor-v1`.
3. The executor reads repository paths only from the gateway config and branch
   names only from committed pull metadata; Git runs with fixed argv, hooks off
   and no ambient credentials.
4. Gates, in order: the pinned pull record, current base tip, current head tip
   (branch-backed pulls), the review gate (one distinct non-author signed
   approval, no outstanding request for changes; skipped only when the request
   carries `overrideReview`, which only merges may set),
   `.forkmesh/required-checks.json` read from the reviewed base commit, and
   `.forkmesh/pull-rules.json` from the same commit (`rules_required`; see
   [pull-rules.md](pull-rules.md)). Neither of the last two can be waived.
5. Merge: branch-backed pulls merge the current tips with `git merge-tree
   --write-tree`; patch-backed pulls (web and cross-node submissions) replay
   `commits.mbox` (then `changes.patch`) onto the exact base in a detached
   worktree under `pull-jobs/`, keeping the contributor's authorship. A pull is
   branch-backed only when its `pull.md` says `derive: branch`; a named `head`
   alone does not make it one. The phones apply the same test when they build a
   merge plan (iOS `pullMergePlan`, Android `PullMergeClient.plan`) and pin the
   ledger commit as the head of a patch-backed pull; before 2026-10-06 they
   treated any pull with a `head` as branch-backed and could not merge a
   contributed pull at all.
6. One `update-ref --stdin` transaction moves the base branch, the
   `forkmesh/pulls` compatibility index, the hidden `refs/pr/<n>/metadata` ref
   (now carrying the merged record), and hidden
   `refs/forkmesh/merge-completed/<token>/{base,pulls}` idempotency refs, and
   deletes the pull's own metadata branch (`refs/heads/forkmesh/pull/<n>`) at
   its exact oid. Since 2026-10-07 a merged pull keeps no branch: the merge on
   `main` is its history. The new `refs/pr/<n>/metadata` commit has the old
   branch tip as its parent, so nothing becomes unreachable, and reward
   verification and number reservation read that ref
   (`docs/server/intake-and-attestation.md`, "Merged pulls keep no branch").
   Before this change the transaction moved the branch to the merged snapshot
   and left it behind forever. The Python executor in
   `tools/headless_mirror_refresh.py` (`_merge_atomic_update`) does the same.
7. It rewrites `expectedRefsSha256` for every gateway entry sharing the Git
   directory, rings the daemon's `/v1/control/sync`, and returns a node-signed
   refs attestation. The relay admits that digest into the pin history and
   pushes a `commits` event so the other mirrors converge. The event goes
   out even when the digest is not admitted, and it reaches every core
   (`docs/CORE_PUSH.md`, "Every core is woken at once").

## Files

- `mirror-gateway/pull-jobs/<sha256(requestId)>.json`: idempotency results,
  kept 30 days, at most 5,000.
- `mirror-gateway/pull-jobs/gateway-generation`: sha256 of the gateway config
  the executor registered; the daemon's cycle compares it so it does not
  restart the gateway over its own no-op write.

## Limits

Request 8 KiB, pull metadata 256 KiB, patch payload 32 MiB, Git steps 5
minutes (constants at the top of `server/merge.go`).

## Operations

- `"disableMergeExecutor": true` in `mirror-node.json` makes a node read-only;
  the gateway then advertises no write capability.
- The capability appears only when both the Go binary and
  `tools/mirror_gateway.py` are new enough; redeploy both.
- A web merge that answers `merge_node_unavailable` usually means no current,
  attested mirror advertises `merge-pull` (old gateway, pin lag after a push,
  or all mirrors inadmissible). Check `/api/repo/<owner>/<repo>/mirrors` and
  the daemon journal.

## Agent branches

Added 2026-09-30 for runner agents on the phone
([../native/RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md)). A runner session's
work sits on `refs/heads/agent/<...>` on the cores, and nothing could merge it
without a pull request, which runner sessions never get. Two more executor
types ride the same job runner, idempotency records, attestation and
generation refresh as the pull mutations (`server/merge_branch.go`):

| Gateway operation | Wire type (App → gateway) | Executor type | Terminal | Failures |
| --- | --- | --- | --- | --- |
| `merge-agent-branch` | `forkmesh.branch-merge-v1` | `forkmesh.branch-merge-executor-v1` | `merged` | `already_merged`, `branch_not_found`, `merge_conflict`, `stale_base`, `stale_head`, `unsupported_branch` |
| `delete-agent-branch` | `forkmesh.branch-delete-v1` | `forkmesh.branch-delete-executor-v1` | `deleted` | `branch_not_found`, `not_merged`, `stale_base`, `stale_head`, `unsupported_branch` |

- **Body.** `{schemaVersion, type, requestId, branch, base, expectedHeadOid}`,
  plus `expectedBaseOid` and an optional `deleteBranch` for a merge, or an
  optional `discard` for a delete. `branch`
  must be under `agent/`; `base` may not be under `agent/` or be the pull
  ledger. A pull body cannot drive a branch operation, nor the reverse: the
  Go parser refuses `pullNumber`, `expectedPullsOid` and `overrideReview` on a
  branch type and `branch`/`base`/`deleteBranch`/`discard` on a pull type
  (and `discard` on a merge), and the
  gateway checks the wire type per operation.
- **Merge.** Both tips must still be the expected ones (`stale_*`). The result
  is always a merge commit (`merge-tree --write-tree`, then `commit-tree` with
  the base and the branch as parents, "Merge branch 'agent/…' into main"),
  like the desktop's merge, so a fast-forwardable branch still shows as one
  merge. A branch already contained in the base is `already_merged`, unless
  `deleteBranch` is set, when it only cleans up.
- **Clean up.** `deleteBranch` (merge) or the delete type removes the branch
  only when its tip is contained in the base on that core (`not_merged`
  otherwise). A delete with `discard` skips that check and removes unmerged
  work too; it still needs the expected tip (`stale_head`), and the tip is
  still saved. The same ref transaction first creates
  `refs/forkmesh/trash/agent-branch/<branch>/<request token prefix>` at the
  old tip, so the tip is saved exactly when the branch goes
  ([destructive-actions.md](../engineering/destructive-actions.md)). The name
  is derived from the request, so a record rebuilt from the completion marker
  after a crash names the same ref.
- **Replication.** The cores fetch every head from each other and never
  delete ([CORE_PUSH.md](../CORE_PUSH.md)), so a removed branch would come
  back from a peer. `reconcileUpstreamRefs` therefore skips creating an
  `agent/` branch whose incoming tip is a trashed tip (or contained in one),
  and the App sends the delete to every core that serves the repository;
  a core whose base has not caught up with the merge answers `not_merged`
  and is listed in `cleanupPending` for the next clean up. New work pushed
  to the same branch name later is not contained in the trashed tip and
  replicates normally.
- **Advertising.** The daemon adds both operations only when the merge
  executor is on and its gateway script (`gatewayScript`) mentions
  `"merge-agent-branch"` (`gatewayScriptSupports`): an older
  `mirror_gateway.py` refuses a configuration with an operation it does not
  know, and the daemon and the script are not always updated together (the
  desktop's host install copies them separately).
- **Receipts.** The App keeps the job in the session record, not in
  `repo_merge_jobs`, so the owner's desktop gets no `mirrorMerges` receipt;
  its mesh convergence fast-forwards onto the new base like after any core
  push.

### Any branch, and updates from the base (2026-10-04)

Asked: "fully manage branches and merge directly from them like we can on
desktop so task all the way to a merged and deployed code should work as it
does on desktop including actions". The two operations keep their names
(`merge-agent-branch`, `delete-agent-branch`) so no advertising, routing or
proof changes, but they now take any user branch:

- `branch` and `base` may each be any Git-legal branch except `HEAD`, a bare
  `agent/` and anything under `forkmesh/` (the pull ledger and per-pull refs),
  and they must differ (`validManagedBranch`, gateway `_managed_branch`).
- **Update from the base** is a merge with the roles swapped: `branch` is the
  base (`main`) and `base` is the branch being updated, so the executor writes
  "Merge branch 'main' into feature" on the feature branch. `already_merged`
  then means the branch is up to date.
- The default branch (`HEAD` of the served repository) is never deleted: a
  delete or a `deleteBranch` merge naming it fails `unsupported_branch`.
- A removed branch outside `agent/` is saved under
  `refs/forkmesh/trash/branch/<branch>/<token>`, and reconcile skips handing
  it back from a peer the same way as an agent branch
  (`agentBranchTombstoned` checks the prefix that matches the branch).
- An older core refuses a non-`agent/` branch as an invalid request; the App
  then queues the operation for the owner's desktop as before
  ([../app/branch-ops.md](../app/branch-ops.md)).

### Hosted repositories merge on their cores (2026-10-04)

The hosted replicator now gives a public hosted repository's gateway entries
the pull mutations and, when enabled, the agent-branch operations. The App
records a successful merge there like a push, so the other cores fetch it
([../HOSTED_REPOS.md](../HOSTED_REPOS.md), "Web merges on a hosted
repository").

### Publishing does not wait for the cache warmup (2026-10-03)

After the executor answers `merged` with `generationReady`, the gateway job
adopts the new generation with `reload_from_disk(prewarm=False)` and warms
its caches on a background thread, then registers. A synchronous warmup walked
the whole repository first and held every pull and branch merge in
`processing` for minutes on a small core, longer than any client polled
([../native/RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md), "Web merge stuck").

### Discarding a run's branch (2026-10-01)

Deleting a runner run on the phone now deletes its branch too, merged or not.
The phone first posts `{"action": "cleanup", "discard": true}` to the
session, then sends the `DELETE`. The App (`agent_branches._cleanup`) reads
the live tip rather than the one a merge attempt recorded, sends
`discard: true` to every core under the request id suffix `-d` (so it never
collides with a plain clean up of the same run), and answers `cleaned` once at
least one core deleted the branch; cores that did not are named in
`cleanupPending`. If no core deleted it yet but at least one core still runs
the delete job (it answered `202`/`processing` to both tries), the answer is
`202 {"status": "processing", "requestId": ...}` and the client polls with the
same request id, as for a merge; the derived per-core request id is stable, so
the poll reads each core's stored result. Only when no core answered at all is
the answer `branch_delete_pending` (503); the phone then keeps the run, so the
branch name is not lost with the session record. The App audits it as
`owner.agent_branch_discard`.

Until 2026-10-04 a still-running job was also answered 503: the Worker asks
each core twice ~50 ms apart, the Go executor runs the delete as a queued job,
and every core answered `202` both times. That 503 raised a new error group
(`POST /api/repo/fmnext-n1/forkmesh/agent-sessions/<id>`, task 1387) and the
phone kept a run whose branch was in fact being deleted.

Clean up stays owner-only, so a member who is not an owner gets `forbidden`;
the phone then deletes the run alone and the branch stays for the owner.

Deploy order: the Go executor and `tools/mirror_gateway.py` refuse a body
with a field they do not know, so the cores need this executor and gateway
before the App sends `discard`. Until then the delete answers `stale_head`
and the phone keeps the run.

Tests: `server/merge_branch_test.go`, `app/tests/test_agent_branch_gateway.py`.

## Traps

- Restarting the gateway kills the job the Worker is polling; the
  generation marker exists to avoid exactly that.
- Nothing rewinds: a stale base, head or ledger pin fails closed
  (`stale_base`, `stale_head`, `pull_not_open`, `review_required`), and the
  owner reloads the pull.
- Reward estimates are settled here too (782d71071), mirroring the desktop.

## Related

- [Pull request rules and model attestations](pull-rules.md)
- [Signed required checks](../../www/docs/operations/required-checks.md)
- [Review threads across patchsets](../../www/docs/design/pull-review-threads.md)
- [server/README.md](README.md)
