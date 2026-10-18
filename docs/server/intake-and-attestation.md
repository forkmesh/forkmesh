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

### Organization aliases

The gateway can list one bare repository under several names: the node owner
(`publishOwner`), the creator or source owner, and organization aliases
(`acme/widgets` linked to `alice/widgets` in `org_repos`). The relay rewrites
an alias to the linked repository before routing, so every name reaches the
same inbox.

- **Catch-up drains each repository once.** `preferredCollaborationAlias`
  picks the first gateway entry for a git directory that is not the node
  owner, or the node owner when there is no other name. It used to accept
  every non-node name, so a hosted repository with an organization alias was
  drained twice per pass.
- **The relay authorizes an alias drain against the linked repository.**
  `_authorized_mirror_issue_signing_key` reads the public URL from the original
  request. The public catalog lists the linked repository, never the alias, so
  the mirror group lookup for `acme/widgets` found nothing and every alias drain
  got `401 unauthorized`. On a single-organization instance this repeated for
  the platform mirror and every hosted repository on each retry. When the
  public name is an organization alias whose `org_repos` row points at the
  routed owner, the lookup (and the flagship check) now uses the routed
  `owner/repo`. An alias linked to a different owner still fails. Tests:
  `TestCollaborationCatchUpDrainsEachRepositoryOnce`,
  `test_mirror_intake_on_an_org_alias_uses_the_linked_repository_group`.

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

### Authors close and delete their own pulls

The person who opened a pull request can close it or delete it, not only the
repository's maintainers (task f9c7d2e2). Both are signed pull events that go
through the pull inbox like a comment:

- `{"type": "status", "state": "closed"}` closes. A reopen is not an author
  event: `status` with any other state fails verification.
- `{"type": "delete"}` deletes.
- The signed content (`pull_comment_content`, `PullStore::contentForSigning`,
  `submitWebPullEvent`) is
  `state + NUL + body` for `status` and `body` for `delete`, inside the usual
  `forkmesh-pull-comment-v2` canonical string.

Who may send one. Both the App (`_authorize_pull_lifecycle` in
`app/src/entry.py`) and the mirror gateway's direct intake
(`_pull_lifecycle_refusal` in `tools/mirror_gateway.py`) read the pull's own
`pull.md` (its `forkmesh/pull/<n>` branch, then `refs/pr/<n>/metadata`, then
`forkmesh/pulls`) and accept the event when:

1. the event is signed by the pull's `author` key (a desktop author signs
   with the same node key that opened the pull); or
2. the signed-in account is the pull's `authorName`, or, on the App, owns the
   pull's `author` key (`_git_push_account_for_pubkey`); or
3. the account manages the repository: the owner, a network admin or an
   organization role with `organization.issues.manage` on the App
   (`_authorize_issue_manager`), the owner on a mirror.

Anyone else gets `403 not_authorized`. A merged, `closed-merged` or already
deleted pull gets `409 pull_final` (a merged record carries reward and payout
data), and an unreadable record `503 pull_unavailable`. A browser's signing key
is per browser, so rule 2 is what lets a web author act; the session name, not
the self-asserted `authorName` in the event, is what is checked, and the queued
event's `authorName` is replaced by it.

What each writer does with an accepted event:

- **Go intake** (`applyPullLifecycleEvent` in `server/intake.go`) writes the
  event file, then rewrites only the `status:` line of `pull.md`: `closed`, or
  `deleted` for a delete, which also removes `changes.patch` and
  `commits.mbox` from the tip (history is not rewritten). A merged or deleted
  record is never changed. `deleted` counts as retired in
  `pullRecordRetired`, so the visible `forkmesh/pull/<n>` branch is retired
  exactly like a merged pull's and the number stays reserved by
  `refs/pr/<n>/metadata`. Keeping a record with a status, instead of removing
  the folder, lets every core converge on it through the normal ledger merge.
- **Qt desktop** (`PullStore::applyRemoteLifecycleEvent`, called from
  `applyPullsInboxPayload`) closes the record with `status: closed` (it does not
  use the owner's own close, which deletes) and deletes with
  `deletePull(number, false)`, never rewriting history for a remote request.
- The web list (`pullMatchesQueryFilters`) and the server-rendered list
  (`plan_pulls` in `app/host/forkmesh_host/repo_views.py`) leave out `deleted`
  pulls.

Where the buttons are. The web pull page shows "Close pull request" (open
pulls) and "Delete pull request" (open or closed pulls) beside the review
buttons to the author and to maintainers (`pullLifecycleAllowed`). The Qt
desktop enables its Close and Delete buttons on a repository it cannot write
when the selected pull was opened with this node's key, and sends the signed
event to the owner's inbox (`requestCurrentPullLifecycle`). The phone apps'
Close and Delete remain requests to the user's own desktop, so they still only
serve repositories that desktop owns.

Tests: `app/tests/test_pull_author_lifecycle.py`,
`server/pull_lifecycle_test.go`,
`test_pull_list_leaves_out_a_pull_its_author_deleted` in
`app/tests/host/test_repo_views.py`.

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
