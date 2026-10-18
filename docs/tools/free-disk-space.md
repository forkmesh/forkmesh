# Freeing disk space

`tools/free_disk_space.py` turns the hand-run disk sweeps of 2026-08 to
2026-10 into one command. It is a dry run unless given `--execute`. The
**Free disk space** Actions workflow (`.forkmesh/free-disk-space.yml`) runs
it with `--execute`, one part per step, so the desktop's Actions page and the
phones' Actions screens show each part as it runs. It replaced the
`free-disk-space.sh` shortcut, which is gone from the shortcut list.

```sh
python3 tools/free_disk_space.py                # list what would go
python3 tools/free_disk_space.py --execute      # delete it
python3 tools/free_disk_space.py --step agents  # one part only
```

## The workflow

Start it with **Run workflow** on the repository's Actions page, or under
**Run by hand** in the iPhone or Android app (the phone asks the paired
desktop to run it). It is manual-only and runs on the desktop outside the
Actions sandbox (`sandbox: false`): a sandboxed step runs as uid 65534 with a
fresh home and `/tmp` and its own `/proc`, so it could not see, let alone
delete, anything this tool cleans. The rules for such workflows are in
[../desktop/README.md](../desktop/README.md#actions-outside-the-sandbox).

| Step | `--step` | What it does |
| --- | --- | --- |
| Measure the disk | `usage` | free space on `/` and `/tmp`, registered worktree count |
| Remove agent leftovers | `agents` | scratch, `builds/agent-NNN*`, `/tmp/fmNNN*`, `~/.cache/fmNNN*` |
| Remove test and Actions leftovers | `tests` | pytest temp dirs, stranded Actions sandboxes, Qt test temp dirs |
| Prune old Claude versions, backups and Trash | `versions` | old Claude Code versions, backups past the 3 newest, trashed cleanup items |
| Clear the pip and npm caches | `caches` | `pip cache purge`, `npm cache clean --force` (skipped with `--no-caches`) |
| Report what is left | `report` | free space after, and the large folders left for the user |

- Every step passes `--repo "$FORKMESH_LOCAL_CHECKOUT"`, the repository's
  local checkout on the desktop, and fails before deleting anything when the
  run has none. The tool keeps agent leftovers by matching them to
  `git worktree list` of `--repo`; run from the Actions clone (its default),
  it would see none of the agent worktrees and treat every live agent's
  builds as orphaned.
- Each step re-reads `git worktree list` and `/proc`, so a worktree or
  process that appears between steps is still respected. A candidate holding
  the tool's own working directory, or its parent shell's, counts as in use.
- A run needs 1 GiB free for its checkout. Below that, run
  `python3 tools/free_disk_space.py --execute` by hand from the primary
  checkout.
- Without `--step` the tool does every part in one pass, as before.

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
minimal archives beyond the 3 newest of each (the keep rule the user set on
2026-10-06); items `clean-forkmesh-temporary-files.sh` moved to the Trash
(`forkmesh-cleanup-*`); and `pip cache purge` / `npm cache clean --force`
(skip with `--no-caches`).

## What is kept

- Anything changed in the last 60 minutes (`--idle-minutes`): a sibling agent
  may be building in it right now.
- Anything a process uses: every `/proc/<pid>` `cwd`, `root`, `exe`, open fd,
  absolute command-line argument and `mountinfo` mount (Actions runs in bwrap
  namespaces, so cwd and fds alone miss them).
- Registered worktrees, even under the folders above (the release worktrees
  under `~/.cache/forkmesh-release-*` and `release/`).
- Everything `forkmesh_safe_delete.remove_tree` refuses; every delete goes
  through it with the expected parent folder.

Never touched, only reported at the end when over 512 MB, because each is a
user decision: Android SDK and AVDs, Gradle homes, `~/Backups`,
`~/Downloads`, `data/backups` (pruned by the desktop's `backup/keepCount`),
`data/actions` (the persistent Actions cache), `release/`, `~/.npm/_npx`,
`go-build`, an idle `~/.cache/ccache` and the Trash.

## Traps

- `/` holds about 240 GB of root-owned data user `f` cannot see, so `du`
  under `/home/f` never adds up to `df`.
- `/tmp` is a separate 16 GB tmpfs: freeing it does not help `/`.
- `ccache`, `ms-playwright`, `go-build` and the Go toolchains are in use, not
  stale; deleting them costs a cold rebuild or a re-download.
- The desktop's hourly backup (`data/backups`) packed the Actions build cache
  and run sandboxes, about 12 GiB a snapshot. On 2026-10-16 a desktop restart
  orphaned a 12 GiB `.part`, cleanup only ran after a successful backup, and
  the disk filled until `tools/next_deploy.py build` failed with ENOSPC (Deploy
  nodes run #3568). The backup now leaves out `data/actions/cache` and
  `data/actions/sandbox`, removes stale `.part` files before each run, skips a
  run when less than 5 GiB is free (`kBackupFreeReserveBytes`, failure code
  `low_disk`), and kills a tar that would take free space below that.
- The disk refills about daily while many agents run. If it hits 0 bytes,
  a write can truncate a source file to 0 bytes (`git checkout -- <file>`)
  and leave a stale `index.lock`.
