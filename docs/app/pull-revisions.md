# Updating a pull request (revisions)

Contributors could not update a pull request. A pull opened from the web, from
a fork or from a `pr/<account>/` branch is a signed snapshot of a patch, and
nothing linked it back to its branch; the contributor guide said to open a new
pull request after pushing more commits. People who rewrote their branch the
GitHub way (amend, rebase) also hit "force push not allowed". Branches on
repositories the cores hold or host can now be force-pushed by whoever may
write them, keeping every replaced tip ([CORE_PUSH.md](../CORE_PUSH.md#force-pushes));
revisions cover the rest: a pull sent as a patch series from someone's own
copy, which has no branch on the cores to push to.

A pull now has revisions. The author signs the branch's current state again and
sends it as a new revision of the same number; comments, reviews and the number
stay. Because the revision is compared fresh, rewritten history needs no force
push: push it to a new branch and update the pull from that branch.

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

## Web

The pull page shows the author of an open patch-backed pull an **Update pull
request** row above the review form, with the source branch prefilled
(`renderPullRevisionPanel`, `submitWebPullRevision` in
`03-issues-profile-io.js`). It compares base against that branch (on the fork
for a fork pull), refuses when nothing changed (`no_new_commits`), signs both
signatures and posts the revision. The timeline shows "updated this pull
request to revision N".

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
