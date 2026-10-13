# Desktop: Git index queue and lock recovery

Part of [the desktop docs](README.md). Code: `desktop/src/GitIndexQueue.{h,cpp}`;
tests: `desktop/tests/test_git_index_queue.cpp` (`forkmesh-git-index-queue-tests`).

## Why

On 2026-09-27 the desktop error log collected about ten Git failures in one
day, all from agents merging and updating worktrees of the same repository at
the same time:

- `Couldn't update <branch> from main in its worktree: fatal: Unable to write index`
- `Unable to create '.../index.lock': File exists. Another git process seems to be running`
- `Could not merge the pull request's branch cleanly: fatal: unable to write index file`
- `previous rebase directory .git/rebase-apply still exists`
- `git timed out after 8s: git merge main`

Three things fed each other. The desktop ran index-writing commands for one
repository from several threads at once (the GUI's pull merge, update-from-main
on workers, commits on main). Every Git wait through `waitForGit` was capped at
8 s, so a large `git merge main` was killed half way, stranding `index.lock`
(or `rebase-apply` for `git am`). And the stale-lock cleanup before a merge
deleted any lock older than 60 s, which also deleted the lock of a long merge
that was still running; that merge then failed with "Unable to write index".

## What the desktop does now

**One queue per repository.** `runGitCaptureDirect` (behind `runGitCapture`)
and PullStore's `runGit` both run through `runGitIndexSerialized`. A command
that writes the index or moves `HEAD` (`merge`, `rebase`, `am`, `cherry-pick`,
`revert`, `pull`, `checkout`, `switch`, `restore`, `reset`, `commit`, `add`,
`rm`, `mv`, `read-tree`, `update-index`, `stash` other than `list`/`show`,
`apply --index/--cached/--3way`) first takes the repository's lane
(`GitIndexWriteGuard`). Reads never queue. The lane is keyed by the common git
dir, so the main checkout and all its linked worktrees share one lane: their
indexes are separate files, but refs, `packed-refs` and the worktree admin
files are shared. Different repositories never wait for each other.

- Worker threads are served in arrival order (a ticket queue).
- The thread that holds a lane can take it again (the GUI thread pumps events
  during a merge and can start a nested Git command for the same repository).
- A GUI-thread waiter pumps events while it waits and goes ahead of queued
  workers. While the GUI thread holds a lane, `runGitCaptureBackgrounded` runs
  Git on the GUI thread instead of handing it to a worker, because a worker
  would wait for a lane the GUI thread cannot release until the worker is done.
- Safety valves, so a stuck holder can never wedge Git for good: a worker waits
  at most 15 s for a lane the GUI thread holds and at most 300 s for any lane,
  then runs anyway and logs `git: index queue for ... still busy`. Git's own
  lock and the retry below then decide.

Commands that use a temporary `GIT_INDEX_FILE` (`runGitCaptureWithEnv`, the
branch-diff `read-tree`/`add` in `runGitStreaming`) do not touch the real index
and stay outside the queue.

**Longer timeout for merges.** `waitForGit` takes `gitWaitTimeoutMs()` (8 s
for reads, 60 s for index-writing commands; see
[diagnostics.md](diagnostics.md)) and passes it through `gitTimeoutMsForArgs`,
which raises `merge`, `rebase`, `am`, `cherry-pick`, `revert` and `pull` to
120 s. PullStore keeps its own 15 s default (60 s for its update-from-base
merge) and gets at least 120 s for the same commands; a caller that already
passes more (history rewrites, 10 min) keeps it. A killed merge is what strands
locks, so the limit is set well above any healthy merge. When a command does
time out, `stopProcessGracefully()` sends SIGTERM first so Git can remove its
own lock.

**Lock contention.** When a command fails with Git's "Unable to create
'...lock': File exists" (or `packed-refs.new`), `GitLockRetryPolicy` decides:

| Lock | State | Action |
| --- | --- | --- |
| `index.lock` | held by a live process | wait 1.5 s, retry once, then report the error |
| `index.lock` | no holder and older than 60 s | remove it, retry at once (logged as `git: removed orphaned ...`) |
| `index.lock` | no holder, younger than 60 s, or already gone | short backoff below |
| any other lock | | up to 5 retries after 25, 50, 100, 200, 400 ms |

`git merge` does not name the lock: when the checkout's `index.lock` exists it
dies with `fatal: Unable to write index.` (its index refresh runs before any
merging). That exact message is treated as `index.lock` contention when the
checkout's `index.lock` exists, and as an ordinary failure otherwise. This is
the "Couldn't update ... from main in its worktree: fatal: Unable to write
index" in the log. The lower-case `unable to write index file` comes from a
command that already held the lock and failed while committing it (typically
because the lock was deleted underneath it); the command may have changed the
worktree, so it is never retried.

Other failures (conflicts, missing refs, timeouts) return at once and
unchanged. `packed-refs.new` counts as contention because
simultaneous branch deletes by agent cleanup jobs collide on it.

**Is a lock held?** `gitLockHeldByLiveProcess`:

- Linux: a process has the lock file open (`/proc/<pid>/fd`), or a process
  named `git` has its working directory in that lock's checkout or git dir.
  The checkout is matched exactly, so Git running in a linked worktree nested
  inside the main checkout (`.worktrees/<name>`) does not hold the main
  checkout's lock.
- macOS and other Unix: `lsof -t` on the lock file; if `lsof` hangs the lock is
  treated as held.
- Windows: not detected; age alone decides, as before. Windows refuses to
  delete a file another process has open, so a live lock survives the attempt.

A lock is removed automatically only when it is both unheld and at least 60 s
old (`removeGitLockIfStale`). This also applies to the orphaned-lock sweep that
runs before a Merge click and before updating a worktree from main
(`removeStaleGitLocks` in `MainWindowBranches.cpp`). The **Clear lock** button
in the branch header still lets the reviewer remove a lock by hand; it asks
first when the lock is younger than 60 s or a running Git process holds it.

## Tests

`forkmesh-git-index-queue-tests` runs against real repositories in a temporary
directory, with the global and system Git config switched off:

- command classification and timeouts (`git merge main` gets 120 s);
- the lane key for a main checkout, a linked worktree and a subdirectory;
- no two index writers of one repository overlap; two repositories do; queued
  writers run in arrival order; the owner can re-enter;
- missing, fresh, stale and held locks, including a lock held open by another
  process and a process named `git` working in the checkout (Linux);
- the retry policy for each row of the table above, and for merge's
  `Unable to write index.` with and without an `index.lock`;
- four linked worktrees updating from main while main takes commits, all
  through the queue: every command succeeds and none overlap; an orphaned
  `index.lock` is cleared by the merge; a briefly held lock is waited out; a
  lock that stays held fails after one retry and is kept.

```sh
cmake --build build --target forkmesh-git-index-queue-tests
./build/forkmesh-git-index-queue-tests
```
