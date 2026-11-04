# Freeing disk space

Part of [the tools docs](README.md).

`tools/free_disk_space.py` frees disk space on a developer box in one command.
It is a dry run unless given `--execute`. Every account's Workflows list has a
**Free disk space** workflow that runs it on the desktop with `--execute`, one
part per step. (It replaced the `.forkmesh/free-disk-space.yml` Actions
workflow and the `free-disk-space.sh` shortcut; both are gone.)

```sh
python3 tools/free_disk_space.py                # list what would go
python3 tools/free_disk_space.py --execute      # delete it
python3 tools/free_disk_space.py --step agents  # one part only
python3 tools/free_disk_space.py --step agents --json  # and end with ::result::{...}
```

Without `--step` the tool does every part in one pass.

## The workflow

Open **Free disk space** on the Workflows page and press **Run workflow**
while the desktop is open and signed in. The browser runs each step on the
desktop over the encrypted desktop control channel; the server cannot run
these steps ([../app/workflows.md](../app/workflows.md#desktop-steps)). Each
step is its own **Desktop** step type:

| Step | Step type | `--step` | What it does |
| --- | --- | --- | --- |
| Measure the disk | `desktop.diskUsage` | `usage` | free space on `/` and `/tmp`, registered worktree count |
| Remove agent leftovers | `desktop.cleanAgents` | `agents` | scratch, `builds/agent-NNN*`, `/tmp/fmNNN*`, `~/.cache/fmNNN*` |
| Remove test leftovers | `desktop.cleanTests` | `tests` | pytest temp dirs, stranded Actions sandboxes, Qt test temp dirs |
| Prune old versions and Trash | `desktop.cleanVersions` | `versions` | old Claude Code versions, backups past the 3 newest, trashed cleanup items |
| Clear pip and npm caches | `desktop.cleanCaches` | `caches` | `pip cache purge`, `npm cache clean --force` (skipped with `--no-caches`) |
| Report free space | `desktop.diskReport` | `report` | free space after, and the large folders left for the user |

- The desktop answers `diag.prepare` / `diag.run` with action
  `freeDiskSpace` and the step name, one step at a time (a second step while
  one runs gets `busy`), and runs
  `python3 -u <checkout>/tools/free_disk_space.py --execute --step <step>
  --repo <checkout> --json` with a 20-minute limit
  (`MainWindow::runLocalControlFreeDiskSpace`). `<checkout>` is the local
  checkout of `forkmesh/forkmesh`, else the first checkout that has the tool;
  with none the step fails with `no_checkout` before deleting anything. Agent
  leftovers are matched to `git worktree list` of `--repo`, so it must be the
  primary checkout, never the app's own build clone.
- Only the desktop owner's browser or paired phone can ask: the `diag.`
  family is owner-only, the step name must be one of the six, and each step
  needs a fresh one-use `diag.prepare` token. Nothing else reaches the
  desktop's shell this way.
- `--json` ends the output with one `::result::` line: `step`, `freedBytes`
  (for `caches`, the free space gained), `items` removed, and `freeBytes` /
  `totalBytes` of the home file system. The desktop returns it with the last
  200 output lines, which become the step's log on the canvas.
- The run's output in the recent runs list is the bytes all steps freed and
  the free space the last step measured: "Freed 3.2 GB · 120.4 GB free".
- A failed run shows **Send to agent**, which puts the failed step, its
  error, its `--step` command and these docs in the agent prompt
  ([../app/workflows.md](../app/workflows.md#send-a-failure-to-an-agent)).
- Each step re-reads `git worktree list` and `/proc`, so a worktree or
  process that appears between steps is still respected. A candidate holding
  the tool's own working directory, or its parent shell's, counts as in use.
- Selecting a step shows it as **Part of a script**: the command, the dry run
  and the functions it runs, with a source viewer
  ([../app/workflows.md](../app/workflows.md#step-details-and-scripts)). The
  map is `workflow_desktop.FUNCTIONS`; when you rename or split a function
  here, update it, or
  `test_each_desktop_step_names_the_part_of_the_script_it_runs` fails.

## What is removed

Agent debris, matched to `git worktree list` of the primary checkout:

- `src/.worktrees/.scratch/<name>` with no worktree of that name;
- `~/projects/forkmesh/builds/agent-NNN*`, `/tmp/fmNNN*`, `/tmp/cmNNN*` and
  `~/.cache/fmNNN*` whose number no worktree carries. Non-numbered
  `builds/agent-<name>` folders (`agent-salvage`, `agent-excl`) are kept.

Test and CI leftovers: `/tmp/pytest-of-$USER/pytest-N` (read-only Go module
caches inside are made writable first), stranded
`actions/sandbox/forkmesh-run-*` / `forkmesh-discard-*` trees, and
`~/.qttest/share/ForkMeshTests/*qt_temp-*` from killed window-test runs.

Other: Claude Code versions in `~/.local/share/claude/versions` except the
newest and any a running process executes; `~/Backups/forkmesh` fleet and
minimal archives beyond the 3 newest of each (the user's keep rule); items
`clean-forkmesh-temporary-files.sh` moved to the Trash
(`forkmesh-cleanup-*`); and the pip and npm caches.

## What is kept

- Anything changed in the last 60 minutes (`--idle-minutes`): a sibling agent
  may be building in it.
- Anything a process uses: every `/proc/<pid>` `cwd`, `root`, `exe`, open fd,
  absolute command-line argument and `mountinfo` mount (Actions runs in bwrap
  namespaces, so cwd and fds alone miss them).
- Registered worktrees, even under the folders above (the release worktrees
  under `~/.cache/forkmesh-release-*` and `release/`).
- Everything `forkmesh_safe_delete.remove_tree` refuses; every delete goes
  through it with the expected parent folder
  ([../engineering/destructive-actions.md](../engineering/destructive-actions.md)).

Never touched, only reported when over 512 MB, because each is a user
decision: Android SDK and AVDs, Gradle homes, `~/Backups`, `~/Downloads`,
`data/backups` (pruned by the desktop's `backup/keepCount`), `data/actions`
(the persistent Actions cache), `release/`, `~/.npm/_npx`, `go-build`, an
idle `~/.cache/ccache` and the Trash.

## Traps

- `/` holds about 240 GB of root-owned data user `f` cannot see, so `du`
  under `/home/f` never adds up to `df`.
- `/tmp` is a separate 16 GB tmpfs: freeing it does not help `/`.
- `ccache`, `ms-playwright`, `go-build` and the Go toolchains are in use, not
  stale; deleting them costs a cold rebuild or a re-download.
- The desktop's hourly backup (`data/backups`) once packed the Actions cache
  and sandboxes (about 12 GiB a snapshot) and an orphaned `.part` filled the
  disk. It now leaves out `data/actions/cache` and `data/actions/sandbox`,
  removes stale `.part` files before each run, skips a run when less than
  5 GiB is free (`kBackupFreeReserveBytes`, failure code `low_disk`), and
  kills a tar that would take free space below that.
- An Actions run checks free space after sweeping stranded sandboxes and
  before the checkout, and fails when less than the workspace quota (2 GiB)
  is left; its message names this workflow. Space often comes back within the
  hour once sibling agent builds finish.
- The disk refills about daily while many agents run. At 0 bytes a write can
  truncate a source file to 0 bytes (`git checkout -- <file>`) and leave a
  stale `index.lock`.
