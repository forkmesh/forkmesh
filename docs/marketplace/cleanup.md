# Disk Cleanup package

`marketplace/packages/cleanup` is a desktop rail page (Market id `cleanup`,
entry point `builtin:cleanup`, off by default) that lists everything Claude
Code, Codex and agent runs leave on this computer, says what each item is
still attached to, and deletes what the user confirms.

Agent sessions fill the disk with transcripts, scratchpads, worktrees,
per-agent build trees, Actions sandboxes and caches. The package answers,
per item, whether a process still has it open, whether an agent or task
still points at it, and whether a worktree holds unmerged commits or
uncommitted edits, and deletes only through the guarded paths
([destructive actions](../engineering/destructive-actions.md)).

## Code map

| Path | Responsibility |
| --- | --- |
| `content/desktop/CleanupScanner.{h,cpp}` | `scan(Context)`: finds, sizes and classifies items. `readProcesses()` snapshots `/proc` (cwd, exe, command line, open files). `liveClaudeSessions()` decides which Claude sessions are running. |
| `content/desktop/CleanupPlan.{h,cpp}` | `buildPlan()` from ticked items and per-item choices; `memberRefusal()` / `worktreeRefusal()` re-check at delete time; `deleteFiles()`, `deleteSandboxes()`; `cleanup.log`. |
| `content/desktop/CleanupFeature.{h,cpp}` | The page: tree by category, filters, confirmation dialog, deletion queue, ccache trim, top-search hits. |
| `desktop/src/CleanupHostFeed.{h,cpp}` | Host socket: repositories, agent sessions, task work items, Actions sandbox folder and claims, worktree removal, sandbox removal, log. |
| `desktop/src/MainWindowCleanup.cpp` | `MainWindow::installCleanupHost()`: answers the socket from the window's in-memory sessions, tasks and repositories. |
| `desktop/src/ActionRunner.{h,cpp}` | `claimedSandboxNames()`, `removeUnclaimedSandbox()`: the claim check and guarded delete the Actions sweep already used, for one tree. |

The scan runs on a worker (`QtConcurrent`) and can be stopped. The page scans
when it is opened and on **Refresh**; it has no timers. `prepareSearch()` is
empty on purpose: a scan can walk tens of gigabytes, so typing in the top
search box never starts one; hits come from the last scan.

## What it lists

Every item has a category, size (disk blocks, hard links counted once, links
never followed), age (newest change inside), a status and the reasons.

- **Claude Code:** `~/.claude/projects/<project>` transcripts and session
  folders (attached while the project folder in the newest transcript's
  `cwd` exists; sessions older than 30 days are a separate orphaned item;
  `memory/` is never a member), the other `~/.claude` caches, `/tmp/claude-<uid>/`
  scratchpads, `~/.cache/claude*`, old CLI versions (the one `claude`
  resolves to is protected), and the ForkMesh agent-account history (always
  protected).
- **Codex:** `~/.codex` sessions, logs (in use while Codex runs), caches,
  temp and snapshots, generated images (confirm), old releases.
  `auth.json`, `config.toml`, memory and goal databases, skills and rules are
  never listed.
- **Agent worktrees** under `<repo>/.worktrees`, `<repo>/.claude/worktrees`,
  `~/projects/forkmesh/worktrees` and `<tmp>/forkmesh-worktrees`, matched to
  their repository; each records branch, commits ahead/behind `main`,
  uncommitted files (`git status --porcelain` with `GIT_OPTIONAL_LOCKS=0`),
  lock state, agent sessions and task work items. A folder with its own
  `.git` folder is a clone and protected.
- **Builds and temp files:** per-agent build trees (attached while the agent
  or its worktree exists), agent scratch in the temp folder, test temp
  folders, Actions sandboxes (in use while a run claims them), the Actions
  build cache (report only), Playwright browsers (newest attached), and
  ccache (the active cache is report only).
- **Other caches:** operator caches need a per-item confirm; release timers'
  folders and anything with a `.git` are protected.

## Status

- **In use**: a process has its cwd, executable or an open file inside it, an
  agent session using it is running or waiting, an Actions run claims it, or
  (for Claude data) the session is live.
- **Attached**: something still points at it: an agent session, a task work
  item, uncommitted files, commits not in `main`, a lock, an existing project
  folder, or a change in the last 30 minutes.
- **Orphaned**: nothing references it.
- **Protected**: never deletable here.

The process check reads `/proc`, so it works on Linux only. On macOS and
Windows, agent sessions, Actions claims and the 30-minute change window are
what mark an item in use; `lsof` was left out because one call per item
would take longer than the scan itself.

A Claude session is live when its id appears on a running `claude` command
line, its transcript changed in the last 30 minutes, or it is among the
newest transcripts of a project a running `claude` has as its cwd (one per
process). Live members are left out of an item's deletable members, so the
current session's scratchpad is never offered.

## Deleting

Only Orphaned and Attached items can be ticked. **Delete selected** opens a
dialog listing each item with its reasons: orphaned ones start ticked,
attached ones and operator caches need their own tick. For a worktree it
offers **Discard changes** (only when it has uncommitted files) and **Retire
branch**. The dialog shows the total that will be reclaimed.

- **Files and folders**: every member goes through
  `forkmesh::safedelete::removeTree(member, itemFolder)`. Right before each
  delete the member is checked again on a fresh `/proc` snapshot and live
  session list: it must be strictly inside the item's folder with no link in
  between, no process may have it open, its session must not be live, and
  nothing in it may have changed in the last 30 minutes. A link is unlinked,
  never followed.
- **Actions sandboxes**: `ActionRunner::removeUnclaimedSandbox()`, which
  checks the claim and deletes with `removeTree(..., Allow::OwnWorktree)`
  under the sandbox folder, as the startup sweep does.
- **Worktrees**: through the host, never `rm`. It releases the sessions that
  track the path (`releaseTrackedWorktree`) and, off the GUI thread, checks the
  path is a linked worktree of the repository. A clean worktree is removed
  with plain `git worktree remove`, so git refuses if anything changed since
  the scan. With **Discard changes**, tracked edits are first saved with
  `git stash create` to `refs/forkmesh/trash/cleanup/<date>/<branch>-wip`
  (untracked files are not saved; the dialog says so), then the desktop's
  existing `removeWorktreeAndBranch()` (the Merge & clean up path) removes it.
  The branch is kept unless **Retire branch** is ticked: then its tip is
  written to `refs/forkmesh/trash/cleanup/<date>/<branch>` and only after
  that is `refs/heads/<branch>` deleted with `git update-ref -d` against the
  same tip. `main`, `master`, the default branch and the branch checked out in
  the main checkout are never retired. Restore with
  `git branch <name> refs/forkmesh/trash/cleanup/<date>/<name>`.

Every removal and every refusal is appended to `<logs>/cleanup.log` (tab
separated: time, `removed` or `kept`, bytes, path, reasons, error), and a
summary line goes to the desktop log. The page then shows "Freed X" and
scans again.

**Trim ccache** runs `ccache --evict-older-than 14d` on the active cache
folder after a confirm.

## Tests

`forkmesh-cleanup-tests` (`content/desktop/tests/test_cleanup_scanner.cpp`)
builds a fake home, temp folder, `/proc` and git repository with worktrees
under `/tmp/forkmesh-cleanup-test-*`. Before anything runs it points `HOME`
and `XDG_DATA_HOME`/`XDG_CONFIG_HOME`/`XDG_CACHE_HOME` at that root, drops
`FORKMESH_HOME`, `FORKMESH_PROJECTS_DIR` and the other ForkMesh folder
variables, and stops if the root is inside the real home. It covers sizing,
the `/proc` reader, every category's classification (including a real
`sleep` process inside a worktree), the plan rules, delete-time re-checks,
links, the log, and the sandbox removal hand-off.
