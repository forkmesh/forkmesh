# Commenting on and closing pulls from the terminal

`tools/pull_ledger.py` lets an operator or an agent read a native ForkMesh pull
request, post a signed comment, close it and reopen it without the desktop.
Every write is a dry run that prints the exact files and commit messages it
would write, unless given `--execute`.

```sh
python3 tools/pull_ledger.py show 315
python3 tools/pull_ledger.py comment 315 --body "Rebased on main, please retest."
python3 tools/pull_ledger.py comment 315 --body-file note.md --execute
python3 tools/pull_ledger.py close 315 --comment "Superseded by #320. Reply here if you disagree." --execute
python3 tools/pull_ledger.py reopen 315 --execute
python3 tools/pull_ledger.py merged 315 --comment "Merged into main as 8d5b213776." --execute
```

`close` and `merged` take `--comment` or `--comment-file`; the comment is written first,
then the status. It refuses an unknown pull, a merged pull, a pull that is
already closed, and `reopen` refuses a pull that is not closed.

## Close keeps the pull

The desktop's own close (`PullStore::setStatus("closed")`) deletes the pull
and rewrites the ledger history, so the contributor never sees why it was
closed. This tool only changes `status: closed` in `pull.md` and keeps the
pull, its events and its history. The contributor can read the closing note
and answer it, and `reopen` puts it back to `open`.

A closed pull stays closed. `PullStore::reconcileMergedPulls` only looks at
`open` and `merged` pulls, the desktop shows `closed` as its own status (`pullDisplayStatus` in
`PullStore.h`), and the web reader passes the status through as the pull's
state (`app/host/forkmesh_host/repo_views.py`), so the dashboard lists it under
"Closed".

## Merged records a merge that already happened

`merged` is for an open pull whose change is already on its base, landed by
hand (`git am` of its `commits.mbox`) or through another pull. The desktop's
Merge button would try to apply the patch again and stop on conflicts, and
`PullStore::reconcileMergedPulls` only recognises branch-backed pulls, so
patch-backed pulls landed this way stayed `open`. Closing them instead would
drop the merge from the record badges and leave a merge-conditional reward
waiting.

It writes what `PullStore::mergePull` writes for a patch-backed pull:
`status: merged`, `mergedAt` (milliseconds) and `mergedBy` (the node key), and
it moves a `rewardStatus: pending_merge` estimate whose condition is empty or
`merged` to `pending_wallet` (`PullReward::advanceForMerge`). It does not
check that the change is on the base; the operator does that first.

A pull that only its own `forkmesh/pull/<N>` branch carries (local, then
`origin`'s fetched copy) and that the ledger has never had is first copied
into the ledger as `pull #N: adopt`. A merged pull keeps no branch
([intake-and-attestation.md](../server/intake-and-attestation.md), "Merged
pulls keep no branch"): the tool commits the merged record to
`refs/pr/<N>/metadata` with the old branch tip (or `origin`'s) as its parent
and deletes the local `refs/heads/forkmesh/pull/<N>` at its exact oid. The
served branch is retired by the server's sweep once the ledger syncs.

## What it writes

The files and commit messages match the desktop's
(`desktop/src/PullStore.cpp`):

- a comment is `.forkmesh/pulls/<N>/<NNNN>-comment.md`, numbered after the
  highest existing event, with the same front matter lines in the same order,
  committed as `pull #N: comment`;
- the status change rewrites only the `status:` line of `pull.md`, committed
  as `pull #N: closed` or `pull #N: open`. The pull's signature covers the
  title, branches and change, not the status, so it stays valid.

The comment is signed with the node identity key
(`forkmesh_paths.resolve().identity_key`, or `--identity-key`). The signed
bytes are `PullStore::canonicalString(number, ev)`:

```
forkmesh-pull-comment-v2\n<owner>/<name>\ncomment\n<N>\n<author>\n<ts>\n<sha256 hex of body>
```

`app/src/events.py:verify_pull_comment_event` checks the same bytes. The
repository key comes from `remote.origin.url` (`forkmesh/forkmesh` for this
checkout) or `--repo owner/name`. `authorName` is the desktop account name
(`account/nodeName` in the settings file) unless `--author-name` is given.

## Writing next to the running desktop

The desktop writes the same ledger at any time, so the tool never commits
through a checkout. It reads the tip of `refs/heads/forkmesh/pulls`, builds
the new tree in a temporary index (`GIT_INDEX_FILE` in a temporary folder),
makes the commits with `commit-tree` and moves the branch with
`git update-ref refs/heads/forkmesh/pulls <new> <old>`. If the desktop
committed in between, the update fails and the tool starts again from the new
tip, up to five times. A close with a comment lands as one move of the branch.
Commits use the git identity in effect, like the desktop's own ledger commits.
It then advances `refs/heads/forkmesh/pull/<N>` and `refs/pr/<N>/metadata`
with a `pull #N: metadata snapshot` commit, as `PullStore::commit` does.

The desktop reads pulls from its `pull-meta` checkout of `forkmesh/pulls`
(`data/pull-meta/<key>`) and commits from it with
`git commit -- .forkmesh/pulls`. It brings that checkout up to date with the
branch only once per run. Left stale, its next ledger commit would put back
the old `pull.md` and drop the new comment. So after a successful write the
tool checks out the files it wrote into every worktree that has
`forkmesh/pulls` checked out, and only when that checkout has no changes of
its own to those files. Otherwise it warns and leaves the checkout alone.
`--no-sync-checkout` skips this step.

Tests: `python3 -m pytest tools/test_pull_ledger.py`. They run against a
temporary repository with a throwaway key and check the signature with the
app's verifier.
