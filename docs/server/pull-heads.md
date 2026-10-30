# Pull requests are commits

A pull request opened from the web, from another node or from the MCP tool
arrives as a signed patch: `changes.patch` and a `git format-patch` series in
`commits.mbox`. Until 2026-10-19 that patch was also how the pull was checked
and merged: the merge executor ran `git am --3way` onto the *current* tip of
the base branch, and the desktop decided its "conflicts" badge with
`git apply --check --3way` while its Merge button merged `refs/pr/<n>/head`.
That broke in ways that kept community pull requests from landing:

- a series whose early commits touch lines the base has since changed failed,
  even when the finished change merges cleanly;
- renames and moved directories on the base made every hunk miss;
- the badge and the merge read different sources, so fixing a conflict meant
  fixing it twice;
- a patch-backed pull merged as a rebase with a new committer, not as a merge,
  and recorded no `mergeBase`/`mergeHead`.

The patch is now only the transport. Every node turns it into commits once, in
the same way, and from then on a patch-backed pull is checked and merged like a
branch: a three-way merge of its head commit into the base tip.

## The head commit

`server/pull_head.go` (Go) and `PullStore::materializePullRef` (desktop) build
the head the same way:

1. Pick the commit to replay onto, in this order: the recorded `headBaseOid`,
   the author's `creationBaseOid`, the newest commit of the base branch whose
   tree holds every pre-image blob named by the patch's `index` lines (searching
   the last 200 commits that touched those files), then the base tip.
   **A candidate is used only when it is an ancestor of the base branch tip.**
   `creationBaseOid` is not covered by the author's signature, so without this
   rule a pull could name the tip of an unrelated branch and its merge would
   bring that branch's history in with it.
2. Replay `commits.mbox` there (a pull with only `changes.patch` gets a
   one-commit synthetic mbox: `From: <authorName> <<author>@forkmesh>`, the
   pull's `ts` in UTC, the title as subject, no description).
3. The replay is deterministic, so every core and the desktop arrive at the
   same object ids without copying objects between them: committer
   `ForkMesh <pulls@users.noreply.forkmesh.com>`,
   `--committer-date-is-author-date`, `--no-message-id --no-scissors
   --no-keep-cr --no-gpg-sign`, hooks off, no global or system Git config on the
   Go side. `TestPullHeadReplayMatchesTheDesktopVector` (Go) and the
   "pull head vector" block in `desktop/tests/test_crypto.cpp` pin the same
   commit ids, so the two implementations cannot drift apart unnoticed.

## Where it is recorded

- **Intake** (`commitPullIntake` in `server/intake.go`, both the relay drain and
  direct intake) builds the head before committing the new record and adds two
  fields to `pull.md`: `headBaseOid` (the commit it replayed onto) and `headOid`
  (the result). It points `refs/pr/<n>/head` at the head. A patch that applies
  to none of the candidates is recorded without them; the merge then reports
  `merge_conflict`, as before. Neither field is part of the author's signature.
- **Other cores** never receive `refs/pr/*`: peers replicate `refs/heads` and
  `refs/tags` only, and refs digests cover only those. Each fetch cycle
  `ensurePullHeadRefs` reads the open patch-backed records on
  `forkmesh/pulls`, and for every `headOid` it lacks replays the series onto
  `headBaseOid` and publishes `refs/pr/<n>/head` only when the result is that
  exact commit. At most four replays per repository per cycle; a mismatch is
  remembered for the life of the process and not retried.
- **Desktop.** `readPull`/`writePull` carry both fields. A pull that arrives
  through the relay inbox has them cleared (`applyRemotePull`): only a node's
  own ledger may name a head. A pull whose content the owner changes (removing
  a file, finishing a conflict fix) has `headOid` cleared and, after a fix,
  `headBaseOid` set to the commit the fix was rebased on.

## Checking and merging

- **Merge executor** (`executeMerge` in `server/merge.go`). The request still
  pins the ledger commit as the head of a patch-backed pull, so the reviewed
  bytes are exactly the pinned `commits.mbox`/`changes.patch`. The executor
  rebuilds the head from those bytes (it never trusts a recorded `headOid`),
  then merges it into `expectedBaseOid` with `git merge-tree --write-tree`:
  a fast-forward when the head sits on the tip, a "Merge pull request #n" commit
  otherwise. The merged record gains `mergeBase` and `mergeHead` like a
  branch-backed pull, and `coAuthors` reads the replayed commits.
- **Desktop** `checkMergeable` and `mergePull` use one head
  (`PullStore::pullHeadCommit`): the live branch for a branch-backed pull,
  otherwise the recorded `headOid` (accepted only when `headBaseOid` is an
  ancestor of both the base tip and the head), otherwise `refs/pr/<n>/head`.
  The badge is a `merge-tree` of that commit, the merge is a `git merge` of that
  commit, and `mergePull` builds the head first when none exists. The old
  `git apply --check` and `git am` paths remain only for a pull that has no
  head commit, because its patch applies to none of the candidate bases.
- **Web.** The pull page's readiness trial (`loadRepoPullReadiness`) also runs
  for a patch-backed pull with a `headOid`, so it shows conflicts and the
  behind count, and the merge control stays hidden while the trial reports a
  conflict.

## Not changed

- The submission wire format, the author signature and the merge request
  format are the same, so older clients and the phones keep working.
- The Python executor (`tools/headless_mirror_refresh.py`) still merges only
  `derive: branch` pulls.
- Required checks still lock a patch-backed pull on a policy branch: the
  attestation names a head commit. Instance runners now run `pull_request`
  workflows on `refs/pr/<n>/head` ([instance-actions.md](instance-actions.md#pull-requests)),
  but do not sign attestations yet.
- "Update branch" on the web is still branch-backed only. The author replaces a
  patch-backed pull's patch with a revision instead
  ([../app/pull-revisions.md](../app/pull-revisions.md)); the head is rebuilt the
  same way.

