# Desktop: Git index queue and lock recovery

Part of [the desktop docs](README.md). Code: `desktop/src/GitIndexQueue.{h,cpp}`;
tests: `desktop/tests/test_git_index_queue.cpp` (`forkmesh-git-index-queue-tests`).

## Why

Agents merge and update worktrees of the same repository concurrently. Without
coordination, parallel index writers collide on `index.lock`, a short timeout
kills a large merge half way and strands the lock, and an age-only stale-lock
sweep can delete the lock of a merge that is still running.

## One queue per repository

`runGitCaptureDirect` (behind `runGitCapture`) and PullStore's `runGit` go
through `runGitIndexSerialized`. A command that writes the index or moves
`HEAD` (`merge`, `rebase`, `am`, `cherry-pick`, `revert`, `pull`, `checkout`,
`switch`, `restore`, `reset`, `commit`, `add`, `rm`, `mv`, `read-tree`,
`update-index`, `stash` other than `list`/`show`, `apply --index/--cached/--3way`)
first takes the repository's lane (`GitIndexWriteGuard`). Reads never queue.

- The lane is keyed by the common git dir, so a checkout and its linked
  worktrees share one lane; different repositories never wait for each other.
- Workers are served in arrival order; the holder can re-enter.
- A GUI-thread waiter pumps events and goes ahead of queued workers. While the
  GUI thread holds a lane, `runGitCaptureBackgrounded` runs Git on the GUI
  thread to avoid a deadlock.
- Safety valves: a worker waits at most 15 s for a lane the GUI thread holds and
  300 s for any lane, then runs anyway and logs `git: index queue for ... still busy`.
- Commands using a temporary `GIT_INDEX_FILE` stay outside the queue.

## Timeouts

`waitForGit` takes `gitWaitTimeoutMs()` (8 s for reads, 60 s for index writes;
see [diagnostics.md](diagnostics.md)) and `gitTimeoutMsForArgs` raises `merge`,
`rebase`, `am`, `cherry-pick`, `revert` and `pull` to 120 s. Callers that pass
more keep it. On timeout `stopProcessGracefully()` sends SIGTERM first so Git
can remove its own lock.

## Lock contention

When a command fails with "Unable to create '...lock': File exists" (or
`packed-refs.new`), `GitLockRetryPolicy` decides:

| Lock | State | Action |
| --- | --- | --- |
| `index.lock` | held by a live process | wait 1.5 s, retry once, then report the error |
| `index.lock` | no holder and older than 60 s | remove it, retry at once (logged as `git: removed orphaned ...`) |
| `index.lock` | no holder, younger than 60 s, or already gone | short backoff below |
| any other lock | | up to 5 retries after 25, 50, 100, 200, 400 ms |

`git merge` reports a held `index.lock` as `fatal: Unable to write index.`; that
message counts as contention only when the checkout's `index.lock` exists. The
lower-case `unable to write index file` means a command failed while committing
its own lock and may have changed the worktree, so it is never retried. Other
failures return unchanged.

`gitLockHeldByLiveProcess` detects a holder via `/proc/<pid>/fd` or a `git`
process whose working directory is that exact checkout (Linux), `lsof -t`
(macOS and other Unix; a hang counts as held), and not at all on Windows, where
the OS refuses to delete an open file. A lock is removed automatically only when
unheld and at least 60 s old (`removeGitLockIfStale`), including the sweep
before a Merge click or a worktree update. The **Clear lock** button asks first
when the lock is younger than 60 s or held.

## Tests

`forkmesh-git-index-queue-tests` runs against real repositories in a temporary
directory with global and system Git config disabled: command classification
and timeouts, lane keys, ordering and re-entry, lock states, the retry table,
and concurrent worktree updates through the queue.

```sh
cmake --build build --target forkmesh-git-index-queue-tests
./build/forkmesh-git-index-queue-tests
```
