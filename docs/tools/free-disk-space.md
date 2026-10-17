# Freeing disk space

`tools/free_disk_space.py` turns the hand-run disk sweeps of 2026-08 to
2026-10 into one command. It is a dry run unless given `--execute`; the
shortcut **Free disk space** (`.forkmesh/shortcuts/free-disk-space.sh`) runs
it with `--execute` (pass `--dry-run` to only list).

```sh
python3 tools/free_disk_space.py                # list what would go
python3 tools/free_disk_space.py --execute      # delete it
.forkmesh/shortcuts/free-disk-space.sh          # same as --execute
```

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
