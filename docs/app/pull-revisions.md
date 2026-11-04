# Updating a pull request (revisions)

Part of the [App](README.md). A pull opened from the web, from a fork or from
a `pr/<account>/` branch is a signed snapshot of a patch, with nothing linking
it back to its branch. Branches on repositories the cores hold or host can be
force-pushed by whoever may write them, keeping every replaced tip
([core push](../operations/core-push.md#force-pushes)); revisions cover the rest: a
pull sent as a patch series from someone's own copy, which has no branch on
the cores to push to.

The author signs the branch's current state again and sends it as a new
revision of the same number; comments, reviews and the number stay. Because
the revision is compared fresh, rewritten history (amend, rebase) needs no
force push: push it to a new branch and update the pull from that branch.

## Wire format

A revision is a pull object signed exactly like a new pull
(`forkmesh-pull-event-v2`, title, base, head, patch, commits), plus:

| Field | Meaning |
| --- | --- |
| `revisionSig` | `forkmesh-pull-revision-v2\n<owner>/<repo>\n<number>\n<author>\n<ts>\n<sig>`, signed by the same key |
| `creationBaseOid`, `creationHeadOid` | the compared commits, used to rebuild the head (not signed, checked for ancestry like any pull) |

The pull signature does not cover the number, so `revisionSig` is what stops a
replay of one of the author's signed pulls onto another of their pulls.

`POST /api/repo/<o>/<r>/pulls` `{number, revision}` (`_accept_pull_revision` in
`app/src/entry.py`, `verify_pull_revision` in `app/src/events.py`) refuses a
revision unless: both signatures verify; the pull is open and patch-backed
(`pull_not_open`, `pull_follows_branch`); its `author` key is the pull's author
(`not_authorized`); its `ts` is newer than the stored one (`revision_stale`);
and the session matches the key (`_bind_session_author`). Subscribers get a
notification and Apps a `pull.revised` event.

The inbox item is `{number, revision, event}`. `event` is a plain `revision`
timeline event: a desktop or mirror that predates revisions treats the item as
an event and records just that line, instead of treating an item without
`event` as a new pull and creating a blank one.

## Applying it

The Go intake (`writePullRevision` in `server/intake.go`) and the desktop
(`PullStore::applyRemoteRevision`, drained first in
`applyPullsInboxEntries`) do the same thing:

1. Ignore (and acknowledge) a revision that is already applied (same `sig`),
   older than the record, from another key, or for a closed or branch-backed
   pull.
2. Copy `pull.md`, `changes.patch` and `commits.mbox` to
   `.forkmesh/pulls/<n>/revisions/<k>/`.
3. Rewrite `pull.md` with the revision's title, base, head, ts, sig and
   creation commits, `revision: k+1`, and empty `headOid`/`headBaseOid`; keep
   number, status, author and description (unless the revision brings one).
4. Write the new patch files (dropping `commits.mbox` when the revision has
   none, so a stale series is never replayed over the new patch).
5. Append `NNNN-revision.md` (type `revision`, `state` = revision number, `sig`
   = `revisionSig`) and commit; the head commit is rebuilt (`recordPullHead`,
   `materializePullRef`) and `refs/pr/<n>/head` moves to it.

Since the stored `sig` always matches the stored patch, nothing that reads a
pull changes. A merge request pins the ledger commit, so a revision landing
after review fails the merge as `stale_pull_metadata` instead of merging bytes
nobody reviewed. Review threads anchor to `creationHeadOid`, which the
revision changes, so threads on changed lines show as outdated.

An accepted revision also dismisses every approval in the relay's review
ledger (`branch_protection.dismiss_approvals`,
[branch-protection.md](branch-protection.md)); requests for changes stay. The
ledger's latest state per account is not tied to a head, so otherwise an
author could collect approval on a harmless patch, revise it, and merge into a
protected branch on the old approval. A merge already in flight is still caught by
`stale_pull_metadata`. Signed review events in the pull's own ledger are not
rewritten; reviews made before the revision show as on an older version.

## Web

The pull page shows the author of an open patch-backed pull an **Update pull
request** row above the review form, with the source branch prefilled
(`renderPullRevisionPanel`, `submitWebPullRevision` in
`03-issues-profile-io.js`). It compares base against that branch (on the fork
for a fork pull), refuses when neither the head nor the base changed (`no_new_commits`), signs both
signatures and posts the revision. The timeline shows "updated this pull
request to revision N".

### Fork pulls compare against the parent

The mirror's `compare` diffs from `merge-base(base, head)`, and a fork pull is
compared on the fork. Comparing against the base branch's name therefore used
the fork's own `main`. When that `main` was behind the parent, the patch carried
every parent change since then, the commit series came back empty (`compare`
drops it when the range holds a merge commit, and the parent's `main` is made of
them), and `creationBaseOid` named the fork's old tip, so the pull showed
dozens of files, no commits and conflicts. Updating it from the same branch
then failed as `no_new_commits`, because the bad revision had already recorded
that head.

Opening and revising a fork pull (`fetchPullComparison` in
`03-issues-profile-io.js`) now read the parent's base tip from its branch list
and compare the fork's branch against that commit id; `resolve_commit` accepts
it because it is an ancestor of a branch that was rebased onto the parent. When
the fork cannot see that commit (the branch was not rebased onto the parent's
current tip) the comparison falls back to the base branch name as before. `no_new_commits`
now needs both the head and the base to match the stored `creationHeadOid` and
`creationBaseOid`, so a pull left wrong by the old comparison is fixed by
updating it again from the same branch: the head is unchanged but the base is
now the parent's tip.

Tests: `app/tests/test_pull_revisions.py` (including the browser code run in
node and verified by the relay), `server/pull_revision_test.go`.

## Not done yet

- Only the author can revise; a maintainer's revision would carry a signature
  that does not match the pull's author key.
- Cross-instance pulls (`host/owner/repo:branch`) are revised on the instance
  that sent them, not here.
- Independent dashboards (`tools/mirror_gateway.py`), the SSH gateway spool and
  the phones do not send or apply revisions yet.
- A pull opened through MCP keeps its `clientSig`, which covers the original
  head and ts, so a revised MCP pull cannot be paid out as a microjob.
