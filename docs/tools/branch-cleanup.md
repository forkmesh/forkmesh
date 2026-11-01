# Branch cleanup: pruning and inventory

Two tools keep the primary checkout's branches in check.
`tools/prune_merged.py` removes branches, worktrees and agent sessions that
`main` already contains; `tools/branch_inventory.py` sorts what is left into
buckets and keeps a daily trend. Part of [the tools docs](README.md).

## Pruning merged branches, worktrees and agents

`tools/prune_merged.py` removes what `main` already contains: local branches,
their worktrees, the desktop agent sessions that produced them and, with
`--remote`, the same branches on the core nodes. It is a dry run unless given
`--execute`.

```sh
python3 tools/prune_merged.py --tasks tasks.json               # print the plan
python3 tools/prune_merged.py --tasks tasks.json --execute     # local only
python3 tools/prune_merged.py --tasks tasks.json --remote --execute
```

Easiest: ask a desktop agent to "prune merged branches". It saves the open
task list (`list_tasks` with `status: open`) to a file, runs the tool with
`--tasks`, and completes the tasks whose branches were removed
(`complete_task`). The tool cannot read the Tasks page itself.

### What is pruned

- A local branch whose tip `main` contains (`git merge-base --is-ancestor`),
  deleted with `update-ref -d` against the recorded commit. Its tip is first
  written to `refs/forkmesh/trash/deleted/<branch>`, the tombstone the
  desktop's own delete leaves, and its `forkmesh-mesh/<branch>` copy is
  dropped when the tip contains it. Without the tombstone the desktop's mesh
  converge re-creates the branch from the mirrors' copy
  ([desktop README](../desktop/README.md)).
- Its worktree, removed with `git worktree remove` (never `--force`).
- A desktop agent session that is finished and merged: not running, its branch
  is in `main` and either carried commits of its own (its tip is not on
  `main`'s first-parent line) or ended in `success`. A session whose branch is
  already gone counts when `main` has a `Merge <branch> into main` commit.
  The session moves to `data/agents/recycle-bin/<owner>-<repo>/<id>` with a
  `deleted-at.json` stamp and is counted in `model-outcomes.json`, the same
  record the desktop's **Delete agent** leaves (`AgentStore::recycleSession`).
- With `--remote`: every `origin` branch `main` contains that is not kept and
  no longer exists locally, through `next_fleet.py delete-refs` on every core.

### What is kept

- `main`, `master`, `forkmesh/pulls` and every `forkmesh/pull/<n>` branch.
- `--keep BRANCH`, and every branch attached to an open task in `--tasks`.
- The branch and session of a standing agent (a session with a `teammateId`,
  a scheduled teammate). Its branch has no commits of its own between runs,
  so it always looks merged.
- A branch whose tip is on `main`'s first-parent line (no commits of its own
  yet, as a new agent's branch is) unless its agent session has finished.
- The branch of an agent that is running or has not finished. A stopped agent
  whose branch only points at a `main` commit (it never committed) is kept: its
  work has not started, it is not merged.
- A worktree with uncommitted files, with a process whose working directory is
  inside it, or outside `.worktrees`, `.claude/worktrees` and
  `~/projects/forkmesh/worktrees`. Its branch is kept with it.

The plan prints every kept branch with the reason.

### Remote branches

Cores refuse branch deletes on push (`receive.denyDeletes`,
[core push](../operations/core-push.md)). `next_fleet.py delete-refs` sends one batch
per core over one SSH session, after one IDS expectation per core, and runs the
cores in parallel. Each branch is deleted only while it is still at the listed
commit and that commit is in `main`; a branch that moved is reported and kept.

Every core fetches its upstream, which the cores themselves serve, so a core
that syncs while another still has the branch gets it back. `delete-refs`
therefore repeats the batch until a pass deletes nothing on any core (at most
4 passes).

## Branch inventory

`tools/branch_inventory.py` puts every local branch, and every `origin` branch
with no local copy, in exactly one bucket and saves the result to
`~/projects/forkmesh/logs/branch-inventory/<date>.json`. The next run prints
each count with its change against the newest earlier snapshot, which gives the
integration report its trend. It only reads git and the agent store; it never
merges or deletes.

```sh
python3 tools/branch_inventory.py              # report, save today's snapshot
python3 tools/branch_inventory.py --no-save    # report only
python3 tools/branch_inventory.py --json       # full rows
```

`main`, `master`, `forkmesh/pulls` and `forkmesh/pull/*` are left out.

### Buckets

Checked in this order; the first match wins.

| Bucket | Rule |
| --- | --- |
| merged | 0 commits ahead of `main`, or `git merge-tree` of `main` and the branch gives exactly `main`'s tree (`merged_by_content`) |
| orphaned | its registered worktree folder is gone |
| stale | no commit in 14 days |
| conflicted | `git merge-tree --write-tree` reports conflicts; the files are listed |
| behind | merges cleanly, but files it changed were also changed on `main` since the merge base; merge `main` in and rerun its tests first |
| ready | merges cleanly and touches nothing `main` changed since the merge base |

Branches with a running agent session are marked `[running]`. The snapshot
also counts worktrees, worktrees whose folder is missing, and finished agent
sessions whose branch no longer exists anywhere.

### Why "merged by content"

A branch can be ahead of `main` only by `Merge branch 'main' into …` commits
while its own work has already landed; merging it would add an empty merge
commit, so it counts as merged. `prune_merged.py` still keeps such branches,
because their tips are not ancestors of `main`.

"Ready" is a git fact, not approval. Before a merge, read the branch-vs-base
diff: a branch built on a stale tree (a recovered worktree, or an old
"Merge divergent mirror collaboration intake") can merge cleanly while
removing lines `main` added later.
