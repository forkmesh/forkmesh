# Branch inventory

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

## Buckets

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

## Why "merged by content"

A branch can be ahead of `main` only by `Merge branch 'main' into …` commits
while its own work has already landed; merging it would add an empty merge
commit, so it counts as merged. `prune_merged.py` still keeps such branches,
because their tips are not ancestors of `main`.

"Ready" is a git fact, not approval. Before a merge, read the branch-vs-base
diff: a branch built on a stale tree (a recovered worktree, or an old
"Merge divergent mirror collaboration intake") can merge cleanly while
removing lines `main` added later.
