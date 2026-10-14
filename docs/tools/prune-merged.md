# Pruning merged branches, worktrees and agents

`tools/prune_merged.py` removes what `main` already contains: local branches,
their worktrees, the desktop agent sessions that produced them and, with
`--remote`, the same branches on the core nodes. It is a dry run unless given
`--execute`.

```sh
python3 tools/prune_merged.py --tasks tasks.json               # print the plan
python3 tools/prune_merged.py --tasks tasks.json --execute     # local only
python3 tools/prune_merged.py --tasks tasks.json --remote --execute
```

The easiest route is to ask an agent in the desktop to "prune merged
branches": it saves the open task list (`list_tasks` with `status: open`) to a
file, runs the tool with `--tasks`, and completes the tasks whose branches the
tool removed (`complete_task`). The tool cannot read the Tasks page itself.

## What is pruned

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

## What is kept

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

## Remote branches

Cores refuse branch deletes on push (`receive.denyDeletes`,
[CORE_PUSH.md](../CORE_PUSH.md)). `next_fleet.py delete-refs` sends one batch
per core over one SSH session, after one IDS expectation per core, and runs the
cores in parallel. Each branch is deleted only while it is still at the listed
commit and that commit is in `main`; a branch that moved is reported and kept.

Trap: every core fetches its upstream, which the cores themselves serve, so a
core that syncs while another still has a branch gets it back. `delete-refs`
therefore repeats the batch until a pass deletes nothing on any core (at most
4 passes).
