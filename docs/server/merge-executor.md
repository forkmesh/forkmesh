# Mirror merge executor

`forkmesh-mirror-node --config … --execute-mirror-merge` performs
owner-authorized web merges and pull-branch updates on a mirror, so an owner
can merge while the source desktop is offline. A merge needs an owner; an
update needs only someone who may push the pull's head branch
([app/pull-merge-box.md](../app/pull-merge-box.md#update-branch)). Code: `server/merge.go`,
`server/checks.go`, `server/pull_metadata_refs.go`. Operator view:
[mirror-node.md, "Owner-authorized web merges"](../../www/docs/operations/mirror-node.md#owner-authorized-web-merges).

## Flow

1. The relay (the app on the cores) authenticates the account (an owner
   for a merge, anyone who may push the head branch for an update), signs
   the exact request (expected base, head and ledger object ids), and sends it to one
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
5. Merge: every pull merges a head commit into the current base tip with `git
   merge-tree --write-tree`. A branch-backed pull's head is its branch tip; a
   patch-backed pull's head (web and cross-node submissions) is rebuilt from
   the pinned `commits.mbox` (or `changes.patch`) on the commit it was cut
   from, in a detached worktree under `pull-jobs/`, keeping the contributor's
   authorship ([pull-heads.md](pull-heads.md)). A pull is
   branch-backed only when its `pull.md` says `derive: branch`; a named `head`
   alone does not make it one. The phones apply the same test when they build a
   merge plan (iOS `pullMergePlan`, Android `PullMergeClient.plan`) and pin the
   ledger commit as the head of a patch-backed pull.
   The merged record gains `mergedAt` (milliseconds), `mergeBase` and
   `mergeHead` and, when the merged
   commits or mbox carry `Co-authored-by` trailers, `coAuthors` (sorted,
   lowercased emails, at most 16). The executor is never told who asked for
   the merge, so it writes no `mergedBy`; the relay records the merger for the
   badge sweep instead ([badges.md](../app/badges.md#record-badges)).
6. One `update-ref --stdin` transaction moves the base branch, the
   `forkmesh/pulls` compatibility index, the hidden `refs/pr/<n>/metadata` ref
   (now carrying the merged record), and hidden
   `refs/forkmesh/merge-completed/<token>/{base,pulls}` idempotency refs, and
   deletes the pull's own metadata branch (`refs/heads/forkmesh/pull/<n>`) at
   its exact oid. A merged pull keeps no branch: the merge on `main` is its
   history. The new `refs/pr/<n>/metadata` commit has the old branch tip as
   its parent, so nothing becomes unreachable, and reward verification and
   number reservation read that ref
   ([intake-and-attestation.md](intake-and-attestation.md), "Merged pulls keep
   no branch"). The Python executor in
   `tools/headless_mirror_refresh.py` (`_merge_atomic_update`) does the same.
7. It rewrites `expectedRefsSha256` for every gateway entry sharing the Git
   directory, rings the daemon's `/v1/control/sync`, and returns a node-signed
   refs attestation. The relay admits that digest into the pin history and
   pushes a `commits` event so the other mirrors converge. The event goes
   out even when the digest is not admitted, and it reaches every core
   (`docs/CORE_PUSH.md`, "Every core is woken at once").

## Progress

During `execute` the executor writes one line per phase change to stderr:
`forkmesh-progress {"phase":"<phase>","at":<ms>}`. Phases, in order:
`fetching` (read the ledger, base and head tips), `verifying` (review gate,
or the branch tips for an update), `checking` (required checks and pull
rules; merges only), `testing` (`merge-tree` conflict check, and replaying a
patch-backed head), `committing` (ledger record or update commit), `pushing`
(the `update-ref` transaction) and `publishing` (gateway generation and daemon
wake). Nothing else goes on those lines: no paths, branch names or object ids.
There is no `hooks` phase today because Git runs with hooks off; the name is
reserved in the vocabulary so a node that adds one needs no relay change.

The gateway runs `execute` through `ExternalMergeExecutor.stream`, which reads
stderr on its own pipe as the helper runs (at most 64 lines of 256 bytes;
unknown phases and other output are dropped) and records `claimed` when the
job thread starts and `publishing` while it reloads and registers. Each
`202 processing` answer carries `progress: {phase, startedAt, updatedAt,
steps: [{phase, at}]}` for that request id. An executor object without
`stream` (an older wrapper, the tests' fakes) still runs through `call` and
reports only `claimed` and `publishing`.

The relay (`_https_mirror_merge_result`) keeps only known phases and integer
times (at most 18 steps), and `repository_pull_merge_handler` writes the
latest record onto the in-flight `repo_merge_jobs` row's `result` column,
`{"progress": ...}`, only when it changed and only while the row is
`requested`. Every other reader of that column filters on a terminal status,
and the terminal `UPDATE` replaces the record with the receipt, so no
migration is needed. The browser gets `progress` with `state`, `node` (the
selected mirror) and a fixed `message` on each 202; when that mirror drops out
of the healthy set the `merge_node_unavailable` / `job_node_offline` answer
carries the last stored record with `state: "waiting"`. All of this is behind
the same owner check as the merge itself.

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

Runner agent work sits on `refs/heads/agent/<...>` on the cores
([../native/RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md)). Two more executor
types merge and delete branches without a pull request, using the same job
runner, idempotency records, attestation and generation refresh as the pull
mutations (`server/merge_branch.go`):

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

Further rules:

- `branch` and `base` may be any Git-legal branch except `HEAD`, a bare
  `agent/` and anything under `forkmesh/`, and must differ
  (`validManagedBranch`). A delete accepts any branch name
  `git check-ref-format --branch` accepts, except `agent/` and `forkmesh/...`
  (`validDeletableBranch`). **Update from the base** is a merge with the roles
  swapped. The default branch is never deleted. Removed non-`agent/` branches
  are saved under `refs/forkmesh/trash/branch/<branch>/<token>`. An older core
  refuses non-`agent/` branches and the App queues the operation for the
  owner's desktop ([../app/branch-ops.md](../app/branch-ops.md)).
- Hosted repositories get the same mutations on their cores
  ([../HOSTED_REPOS.md](../HOSTED_REPOS.md)).
- After `merged` with `generationReady`, the gateway adopts the generation
  with `reload_from_disk(prewarm=False)` and warms caches in the background.
  The warmup waits until no job finished for 5 s and runs once at a lower
  CPU priority (`_schedule_prewarm`), so a run of branch deletes costs one
  warmup instead of one per branch; delete jobs themselves run at nice +10
  ([../app/branch-ops.md](../app/branch-ops.md#how-the-job-runs)).
- **Discard.** Deleting a runner run on the phone posts
  `{"action": "cleanup", "discard": true}` before the `DELETE`; the App
  (`agent_branches._cleanup`) sends `discard: true` to every core (request id
  suffix `-d`), answers `cleaned` once one core deleted the branch (others in
  `cleanupPending`), `202 {"status": "processing", "requestId": ...}` while a
  core is still working, and `branch_delete_pending` (503) only when no core
  answered. Owner-only; audited as `owner.agent_branch_discard`.
- The executor and gateway refuse unknown body fields, so deploy them to the
  cores before the App sends a new field.

Tests: `server/merge_branch_test.go`, `app/tests/test_agent_branch_gateway.py`.

## Traps

- Restarting the gateway kills the job the Worker is polling; the
  generation marker exists to avoid exactly that.
- Nothing rewinds: a stale base, head or ledger pin fails closed
  (`stale_base`, `stale_head`, `pull_not_open`, `review_required`), and the
  owner reloads the pull.
- Reward estimates are settled here too, mirroring the desktop.

## Related

- [Pull request rules and model attestations](pull-rules.md)
- [Signed required checks](../../www/docs/operations/required-checks.md)
- [Review threads across patchsets](../../www/docs/design/pull-review-threads.md)
- [server/README.md](README.md)
