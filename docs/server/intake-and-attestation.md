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

Each open pull has its own branch `refs/heads/forkmesh/pull/<n>` (compatibility
alias `refs/pr/<n>/metadata`). `forkmesh/pulls` is kept as a compatibility
index for older clients (`server/pull_metadata_refs.go`, 1cd762723).

### Merged pulls keep no branch (2026-10-07)

The owner: "we don't want to keep pull request history if the pull was merged
into main, that's history enough", and merging or deleting a pull must delete
its branch too. 210 of the 233 per-pull branches on forkmesh/forkmesh belonged
to merged pulls. A merged pull now keeps only the hidden
`refs/pr/<n>/metadata` ref, which is not a branch, so it stays out of branch
lists and clones but still carries the merged record, the reward fields the
payout check reads, and the pull number.

- **Intake.** `publishPullMetadataRefs` reads the record it just wrote. A
  merged record (`status: merged` or `closed-merged`) updates only
  `refs/pr/<n>/metadata` and deletes the branch in the same `update-ref
  --stdin` transaction, so a comment on a merged pull never brings its branch
  back. `materializePullMetadataCommit` takes its parent from the branch, or
  from `refs/pr/<n>/metadata` once the branch is gone, so the history stays one
  line.
- **Sweep.** Every fetch cycle `retireMergedPullBranches` runs once per Git
  directory, after upstream sync and collaboration intake. It deletes
  `refs/heads/forkmesh/pull/<n>`, compare-and-swap on the branch's exact oid,
  when the `forkmesh/pulls` index records the pull as merged and the branch is
  either merged too or is the same pull (same `ts` and `author`) at an older
  `open` snapshot. In the second case it first commits the index's record on
  top of the branch tip. `refs/pr/<n>/metadata` is created or moved to that
  commit in the same transaction, or left where it is when it already contains
  it, and the delete is refused unless the kept ref contains the branch tip.
  The sweep requires a merged index record because the web pull list and the
  mirror gateway fall back to `forkmesh/pulls` once the branch is gone. A
  branch whose own record is merged while the index still says open is kept
  until the index catches up. Open and closed pulls are never touched. This
  cleans up the existing merged branches on each core after deploy.
- **Upstream sync.** `reconcileUpstreamRefs` no longer creates a missing local
  `refs/heads/forkmesh/pull/<n>` when the incoming record and the local or
  staged upstream `forkmesh/pulls` record pass the same rule. Peers and
  external upstreams that still hold the branch would otherwise bring it back
  on every fetch. When the incoming record itself says merged, it is adopted
  into `refs/pr/<n>/metadata` if that ref is missing or older. All candidates
  are read with one `git cat-file --batch`.
- **Numbers.** `reservedPullNumbers` also reserves every `refs/pr/<n>/metadata`,
  so a merged pull's number is never reused even if the index loses it.
- The same rule is in the merge executors (`docs/server/merge-executor.md`)
  and the desktop (`docs/desktop/diagnostics.md`, "Pull list after a ledger
  change from elsewhere"). The payout check in `app/src/ledger.py` reads the
  record from the branch, then `refs/pr/<n>/metadata`, then `forkmesh/pulls`;
  `app/tools/sync_contributor_rewards.py` reads `refs/pr/<n>/metadata` next
  to the branches. The mirror gateway resolves `ref=refs/pr/<n>/metadata` as
  written; since 2026-10-07 `resolve_commit` accepts a full `refs/` name only
  under `refs/heads/`, `refs/tags/`, `refs/pr/` and `refs/remotes/origin/`
  (see [README.md](README.md#security)). Tests: `server/pull_metadata_retire_test.go`,
  `TestMergeExecutorDeletesTheMergedPullBranchInTheSameTransaction`.

## Closed issues removed (2026-10-13)

At the owner's request, the closed issues in `.forkmesh/issues/closed/` and
their attachments were removed on 2026-10-13: 681 folders, 734 files, 8.6 MB.
Every money-bearing issue among them is recorded on the public ledger
(`app/src/ledger.py`, `/api/ledger`), which lists each contributor reward by
`rewardId`, issue number and, once paid, its Solana transaction signature. The
ledger keeps a short title and summary of each issue, but its issue link now
points at a removed issue. Five closed issues were kept:

- #727 and #728 hold `reward-discord-*` estimates (`pending_wallet`) that are
  not on the ledger yet. `app/tools/sync_contributor_rewards.py` reads them
  from these files.
- #126 and #207 hold legacy per-issue bounty escrows that the ledger's
  backfill does not scan. `GaYzEifX…` (#126) still holds 0.015 SOL, and
  `CeJEUGuc…` (#207) paid 0.0151 SOL to the reporter and 0.0017 SOL to the
  treasury.
- #735 is the highest issue number. Every allocator (desktop
  `IssueStore::nextNumber`, the Go intake fallback, iOS `NodeSyncApplier`,
  the catalog's `issueMaxNumber`) takes the highest folder number plus one.
  Removing #735 would hand out numbers that the ledger and old links still
  use.

Another 97 issues carried legacy `bountyUsd` pledges of $1 or $2 ($146 in all) that
were never funded. They were removed. The reporter leaderboard
(`_issue_reporter_tracker_fold`) counts only the issues that remain.
`/home/f/Backups/forkmesh/history-rewrite-2026-10-13/src-all.bundle` holds the
tree as it was before the removal, at commit `ae985af229`, and git history
still has every removed file.

## Divergence

A mirror and its upstream diverge when both accepted intake. The daemon merges
any divergence whose `.forkmesh/issues` trees differ itself
(`reconcileIssueOnlyDivergence`, called from `reconcileUpstreamRefs`); the
source desktop absorbs collaboration-only divergence from the mesh
(`reconcileCollaborationDivergence` in `desktop/src/UpstreamCheckoutSync.cpp`).

When both sides wrote the same `issue-N.json` (issue #683), the desktop merges
the two records instead of giving up. This happens when a mirror gets a claim
for an issue it has never seen: it writes an untitled `relay-placeholder` open,
while the source builds its own copy from the title events it drained. Before
this fix, `git merge-tree` stopped on the add/add conflict. The fleet then
stayed on the mirror's lineage and the web showed "issue #N" as the title.
`mergeIssueRecords` keeps the source record and replaces a placeholder open
with a signed one. It appends the peer events whose ids are new, and applies
each one's projection (title, status, labels, claim and so on) only when no
newer source event of the same kind exists. It bails out if the merged status
no longer matches the `open/` or `closed/` folder.
### The convergence rule (2026-09-28)

On 2026-09-28 the three cores diverged on #729 and one merge returned
`issue identity … has divergent materializations`; the error stopped sync and
the edge refused git for 2.5 h. The rule since then: **issue reconciliation
never returns an error for any history honest cores can write.** The only
errors left are git failures (a process that cannot run, a corrupt object, the
256 MiB output cap). The merge is a pure function of the two sides' issue
records (plus the merge bases, for deletions and one fallback only), so the
same inputs give the same tree on every core, and merging three cores' heads
pairwise in any order, or gossip-style, gives one tree
(`server/issue_converge_test.go`).

How the merged tree is built:

1. **Everything outside `.forkmesh/issues/{open,closed}/<n>/`** comes from
   `git merge-tree --write-tree` of the two heads (git handles several merge
   bases itself). Source changes on both sides therefore merge like any clean
   git merge. If git reports a conflict outside the issue folders (two owners
   pushed conflicting commits to different cores), the non-issue content is
   taken whole from the newer head, by commit time and then object id: the
   same winner `reconcileUpstreamRefs` picks for a conflicting source
   divergence. The other head stays reachable as a parent of the merge commit.
   A clean code-only divergence of `main` merges instead (2026-10-04,
   `mergeDivergentMain`, [../deploy/README.md](../deploy/README.md) "Recent
   changes").
2. **Every issue folder on either side is one copy of an issue.** A copy's
   identity is its opening event: `sig:<signature>`, or
   `unsigned:<author>:<ts>:<title>`. A legacy placeholder (sig
   `relay-placeholder`, or an unsigned opening with no title) has no identity
   and stands for "whatever the relay calls #n". A folder whose record is
   missing, unparsable, over 2 MiB or has no valid opening is kept as it is, at
   its path, and its number is never handed to another issue.
3. **Numbers.** Each identity has a *claim*, the number intake gave it (the
   record's `intakeNumber` when a merge moved it, otherwise its number; the
   lowest over its copies), and a *floor*, the highest number any copy sits at.
   - The winner of number *n* is the identity with the earliest opening
     timestamp (then the smaller identity string) among those whose claim and
     floor are both *n*. It keeps *n*.
   - Every other identity is renumbered, in order of (claim, opening time,
     identity), to the lowest free number at or above both claim+1 and its
     floor. Free means not a winner's number, not a kept placeholder's or
     unreadable folder's number, and not already given out in this pass.
   - A renumbered record gets `intakeNumber` = its claim, so later merges still
     know its original number. An issue at its own claim has no
     `intakeNumber`.
   - So a number only ever moves up: an issue seen at #5 on one core and at #6
     on another ends at #6 or higher, never back at #5, and the copy at #5
     leaves nothing behind in the slot the other issue owns. Because the
     result depends only on the claims, which a merge never changes, three
     cores merged in any order get the same numbers. New intake takes the local
     max plus one, which no pending renumbering can already hold.
   - A placeholder folds into the winner of its number. With no winner it
     stays at its number.
4. **Content of an issue with several different copies** (including folded
   placeholders):
   - Events: the union of all copies, deduplicated by `id` + `sig` (by the
     full event when both are empty), the opening first and the rest sorted by
     (`ts`, key). Placeholder openings are dropped; every other event,
     signed or not, is kept.
   - Replacing fields follow the latest event of their kind by (`ts`, key):
     `title`, `status`, `labels`, `milestone`, `startDate`/`endDate`
     (`dates`), `priority`, `progress`. `status` also decides `open/` or
     `closed/`, so a close on one core and a comment on another converge to
     closed with both events.
   - `assignees`: the latest `assignees` event, or the copies' list without
     any name a claim mentions, then every later `claim` replayed in order.
     `votes`: the count of `vote` events plus whatever the records held
     before any vote event.
   - Any other field the copies disagree on takes the value that differs from
     the merge base (the side that changed it); failing that, the largest JSON
     encoding. `updatedAt` is the latest of all copies and events.
   - Files: the union of every copy's attachments. Two different files at the
     same name are both kept: the smaller blob id keeps the name, the other is
     stored as `<name>~<first 12 hex of its blob id>`.
   - A copy that is identical everywhere is left byte for byte as it was, and
     only moved (with its `number` rewritten) when its number changes.
5. **Deletion.** An identity that is in a merge base but missing from one side
   was removed there by the owner's `delete/self` event, and stays removed
   even if the other side added events to it (they remain in that side's
   history). This is the one case where events leave the tip.

Known limits:

- An event names its issue by number. If a core that has not merged yet
  receives an event for #6 meant for the issue another core moved to #6, it is
  appended to whatever that core has at #6. It then travels with that issue.
- A legacy placeholder folds into the winner of its number at the time of the
  merge. If a still earlier opening for that number appears later, the
  placeholder's events stay with the issue they were first folded into.
- Deletions and the desktop dropping `intakeNumber` (below) are the inputs a
  merge cannot see whole, so for them only pairwise determinism is promised,
  not the same tree for every merge order.
- The desktop's `IssueStore` rewrites records with a fixed key set and drops
  `intakeNumber`. The issue then keeps its current number as its claim: still
  deterministic, but it can compete for that number again.
- The source desktop's own merge (`reconcileCollaborationDivergence`) is
  separate code and does not follow this rule yet.

## Attestation and admission

The relay routes clones only to a mirror whose live refs digest is in the
repository's pin set: the source's current state hash plus attested history
(`clone_state_pins` in `app/src/mirrors.py`). Mirrors add to that history
through intake acknowledgements and merge-executor results. A repository with
no pin at all is not publicly routable.

## Traps

- Intake worktrees check out only `.forkmesh/` (2026-10-07). Every
  intake writer (issues, pulls, discussions, branch reviews) writes under
  `.forkmesh/`. Before this change each intake checked out the whole
  repository with `worktree add`, and n1's daemon wrote 1.7 GB in 11
  minutes. `collaborationWorktree` now adds the worktree `--no-checkout`,
  writes `/.forkmesh/` to the worktree's own `info/sparse-checkout`, and runs
  `read-tree -mu HEAD` with `-c core.sparseCheckout=true`, so the shared
  repository config is untouched. Commits are limited to the `.forkmesh/...`
  pathspec, so files outside it stay in the tree.

- `collaborationItem` is decoded strictly: every key the gateway adds to an
  item must be declared in `server/intake.go`, or every submission fails with
  `invalid_json` (issue #700).
- After a mirror's own reconciler merge, two merge bases are the normal shape.
  `git merge-tree` resolves them for the source side; the issue side needs the
  bases only to spot deletions. Daemons older than 2026-09-26 give up and need
  a manual merge.
- **Never fabricate an issue (2026-09-28).** A non-open event (claim,
  title, comment…) for an issue this node does not have used to invent a
  `relay-placeholder` opening with an empty title. On 2026-09-28 n2 did that
  for #729 when a claim release reached it before the opening had synced, and
  the cores diverged. `writeIssueItem` now returns `errIssueUnknown`: the
  loopback endpoint answers 503 `issue_not_synced` (`retryable: true`,
  `Retry-After: 30`) and the relay-inbox drain leaves that row unacknowledged
  so it lands after the opening does. A signed opening that finds a legacy
  placeholder (sig `relay-placeholder`, or an unsigned opening with no title)
  at its number takes that number and replays the placeholder's later events
  (`foldIssueEvents`) instead of moving to the next free number.
- **A merge never renumbers the same issue.** `reconcileIssueOnlyDivergence`
  used to renumber any addition whose number the other side already held; n3
  turned n2's placeholder #729 into a phantom #730. The same issue (same
  opening identity) or a placeholder is folded, never split; only two
  different openings that raced for one number are, as described in "The
  convergence rule" above. Until 2026-09-28 the merge still failed closed on
  the same issue at two numbers, a conflicting attachment, an open/closed
  move, a comment on an existing issue on both sides, or source changes on
  both sides; all of these now converge.
- **Closing kept only the record (fixed 2026-09-28).** `writeIssueItem` moved
  an issue between `open/` and `closed/` by writing the record in the new
  folder and deleting the old one, so a close through a core dropped its
  attachments. It now moves the folder (`moveIssueDirectory`).
- **A move never deletes (2026-09-29).** When the destination folder already
  exists, `moveIssueDirectory` renames each source entry into it; until
  2026-09-29 an entry whose name was taken was skipped and then deleted with
  the source folder. It is now kept beside the existing one as
  `<name>.conflict-YYYYmmdd-HHMMSS` (`-2`, `-3`, ... on a repeat), the node
  log names both paths, and the emptied source folder is removed with a plain
  `os.Remove`, which fails on anything left in it. `issueFiles` only reads
  `issue-*.json`, so an `issue-7.json.conflict-...` file is never read as a
  record. Test: `TestMoveIssueDirectoryKeepsCollidingEntries`.
- The catalog's "synced" time is the lease, not Git freshness. A fleet can read
  "synced 40 s ago" while hours behind.

## To be filled

Discussion record format, attachment handling (`writeIssueAttachments`), and
the reward fields copied by `copyIssueReportReward`: to be filled from source
comments.
