# Collaboration intake and refs attestation

How web issue, pull and discussion submissions become Git commits on a mirror,
and how the relay learns which refs a mirror serves. Code: `server/intake.go`,
`server/issue_reconcile.go`, `server/pull_metadata_refs.go`,
`server/catalog.go`, `server/identity.go`. Background on convergence with the
source desktop lives in the desktop's `desktop/src/UpstreamCheckoutSync.cpp`.

## Intake

- **Doorbell.** The relay pushes a payload-free event whose topic is
  `issues`, `pulls` or `discussions` and whose `repo` names the inbox. The
  daemon marks that inbox pending (`Daemon.handleNodeEvent`) and clears any
  backoff for it. Every socket reconnect adds a catch-up pass over all inboxes.
- **Lease.** `drainCollaborationKind` leases one bounded page at a time
  (`pending`, `more`) with a node-signed query.
- **Materialize.** A temporary `git worktree` (committer `<node>` /
  `<node>@users.noreply.forkmesh.com`) receives the repository-native record
  (`.forkmesh/issues/…`, pull ledger entries, discussions), which is committed.
- **Direct intake.** A mirror that accepts a submission itself (the gateway,
  on the independent dashboard) posts it to the loopback
  `POST /v1/control/collaboration`; the same writer runs.
- **Acknowledge.** `ackCollaboration` acknowledges exactly the row ids it wrote,
  with a signed `forkmesh-repostate-v1` statement of the refs digest after the
  commit. The relay records that as an attested state for the repository.

## Pull metadata

Each open pull has its own branch `refs/heads/forkmesh/pull/<n>` (alias
`refs/pr/<n>/metadata`). `forkmesh/pulls` is kept as a compatibility index for
older clients (`server/pull_metadata_refs.go`).

### Merged pulls keep no branch

A merged pull keeps only the hidden `refs/pr/<n>/metadata` ref, which stays
out of branch lists and clones but carries the merged record, the reward
fields the payout check reads, and the pull number.

- **Intake.** `publishPullMetadataRefs` reads the record it just wrote. A
  merged record (`status: merged` or `closed-merged`) updates only
  `refs/pr/<n>/metadata` and deletes the branch in the same `update-ref
  --stdin` transaction. `materializePullMetadataCommit` takes its parent from
  the branch, or from `refs/pr/<n>/metadata` once the branch is gone.
- **Sweep.** Every fetch cycle `retireMergedPullBranches` deletes
  `refs/heads/forkmesh/pull/<n>` (compare-and-swap on its exact oid) when the
  `forkmesh/pulls` index records the pull as merged and the branch is merged
  too or is an older `open` snapshot of the same pull (first committing the
  index's record on top). The delete is refused unless the kept ref contains
  the branch tip. Open and closed pulls are never touched.
- **Upstream sync.** `reconcileUpstreamRefs` does not recreate a merged
  pull's branch from peers; a merged incoming record is adopted into
  `refs/pr/<n>/metadata`.
- **Numbers.** `reservedPullNumbers` reserves every `refs/pr/<n>/metadata`, so
  a merged pull's number is never reused.
- The same rule is in the merge executors ([merge-executor.md](merge-executor.md))
  and the desktop. The payout check in `app/src/ledger.py` reads the record
  from the branch, then `refs/pr/<n>/metadata`, then `forkmesh/pulls`. Tests:
  `server/pull_metadata_retire_test.go`,
  `TestMergeExecutorDeletesTheMergedPullBranchInTheSameTransaction`.

## Issue numbers

Every allocator (desktop `IssueStore::nextNumber`, the Go intake fallback,
iOS `NodeSyncApplier`, the catalog's `issueMaxNumber`) takes the highest
issue folder number plus one. Never remove the highest-numbered issue folder,
or numbers already used by the ledger and old links would be handed out
again. Issues holding unsettled reward data
(`app/tools/sync_contributor_rewards.py`) must also stay.

## Divergence

A mirror and its upstream diverge when both accepted intake. The daemon merges
any divergence whose `.forkmesh/issues` trees differ itself
(`reconcileIssueOnlyDivergence`, called from `reconcileUpstreamRefs`); the
source desktop absorbs collaboration-only divergence from the mesh
(`reconcileCollaborationDivergence` in `desktop/src/UpstreamCheckoutSync.cpp`),
merging two records of the same issue with `mergeIssueRecords`.

### The convergence rule

**Issue reconciliation never returns an error for any history honest cores
can write.** The only errors left are git failures (a process that cannot
run, a corrupt object, the 256 MiB output cap). The merge is a pure function
of the two sides' issue records (plus the merge bases, for deletions and one
fallback only), so the same inputs give the same tree on every core, and
merging three cores' heads pairwise in any order gives one tree
(`server/issue_converge_test.go`).

How the merged tree is built:

1. **Outside `.forkmesh/issues/{open,closed}/<n>/`**: `git merge-tree
   --write-tree` of the two heads. On a conflict there (conflicting source
   pushes on different cores), non-issue content is taken whole from the
   newer head (commit time, then object id), the same winner
   `reconcileUpstreamRefs` picks; the other head stays reachable as a parent.
   A clean code-only divergence of `main` merges (`mergeDivergentMain`).
2. **Identity.** Every issue folder is one copy of an issue, identified by its
   opening event (`sig:<signature>` or `unsigned:<author>:<ts>:<title>`). A
   legacy placeholder (sig `relay-placeholder`, or an unsigned untitled
   opening) has no identity. An unreadable record (missing, unparsable, over
   2 MiB, no valid opening) is kept at its path and its number is never
   reused.
3. **Numbers.** Each identity has a *claim* (`intakeNumber` when a merge moved
   it, else its number; lowest over copies) and a *floor* (highest number any
   copy sits at). The winner of *n* is the identity with the earliest opening
   (then smaller identity) whose claim and floor are both *n*; every other
   identity moves, in (claim, opening time, identity) order, to the lowest
   free number at or above both claim+1 and its floor, and records
   `intakeNumber` = claim. Numbers only move up, and results depend only on
   claims, so any merge order gives the same numbers. A placeholder folds into
   the winner of its number.
4. **Content of several copies.** Events are the union deduplicated by `id` +
   `sig`, opening first, the rest by (`ts`, key); placeholder openings are
   dropped. `title`, `status`, `labels`, `milestone`, dates, `priority` and
   `progress` follow the latest event of their kind (`status` also picks
   `open/` or `closed/`). `assignees` replay claims over the latest
   `assignees` event; `votes` count `vote` events. Other disagreeing fields
   take the side that changed from the merge base, else the largest JSON
   encoding. Attachments are unioned; a name clash keeps both, the second as
   `<name>~<first 12 hex of its blob id>`. Identical copies stay byte for
   byte.
5. **Deletion.** An identity in a merge base but missing from one side was
   deleted by the owner (`delete/self`) and stays deleted.

Known limits: events name issues by number, so an event reaching an unmerged
core can attach to the issue that core has at that number; deletions and the
desktop's `IssueStore` dropping `intakeNumber` only promise pairwise
determinism; the desktop's own `reconcileCollaborationDivergence` is separate
code.

## Attestation and admission

The relay routes clones only to a mirror whose live refs digest is in the
repository's pin set: the source's current state hash plus attested history
(`clone_state_pins` in `app/src/mirrors.py`). Mirrors add to that history
through intake acknowledgements and merge-executor results. A repository with
no pin at all is not publicly routable.

## Traps

- Intake worktrees check out only `.forkmesh/`: `collaborationWorktree` adds
  the worktree `--no-checkout`, writes `/.forkmesh/` to the worktree's own
  `info/sparse-checkout`, and runs `read-tree -mu HEAD` with
  `-c core.sparseCheckout=true`. Commits are limited to the `.forkmesh/...`
  pathspec.
- `collaborationItem` is decoded strictly: every key the gateway adds to an
  item must be declared in `server/intake.go`, or every submission fails with
  `invalid_json`.
- After a mirror's own reconciler merge, two merge bases are the normal shape;
  `git merge-tree` resolves them for the source side.
- **Never fabricate an issue.** A non-open event (claim, title, comment…) for
  an issue this node does not have returns `errIssueUnknown`: the loopback
  endpoint answers 503 `issue_not_synced` (`retryable: true`,
  `Retry-After: 30`) and the relay-inbox drain leaves the row unacknowledged
  until the opening lands. A signed opening that finds a legacy placeholder at
  its number takes the number and replays the placeholder's events
  (`foldIssueEvents`).
- **A merge never renumbers the same issue.** The same opening identity or a
  placeholder is folded, never split; only two different openings that raced
  for one number are renumbered.
- **Closing moves the folder** (`moveIssueDirectory`), attachments included.
  When the destination exists, colliding entries are kept as
  `<name>.conflict-YYYYmmdd-HHMMSS` and the emptied source folder is removed
  with a plain `os.Remove`. `issueFiles` only reads `issue-*.json`. Test:
  `TestMoveIssueDirectoryKeepsCollidingEntries`.
- The catalog's "synced" time is the lease, not Git freshness.
