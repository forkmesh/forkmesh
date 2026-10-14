# Destructive actions

## The rule

No code deletes a folder tree, or runs a git command that throws away commits,
branches, stashes or uncommitted work, except through a guarded helper or from
a line on the contract test's allow-list with a written reason.

A failure that may be transient (a network error, a 5xx, a timeout, a failed
`git pull`, `fetch` or `clone`) never leads to a delete, a `reset --hard` or a
re-clone of an existing checkout. The code stops, says what failed, and leaves
everything as it was. When an existing folder must make room, it is moved
aside to a timestamped sibling (`<name>.replaced-YYYYmmdd-HHMMSS`), never
deleted. A clone only goes into a folder that is missing or empty.

The rule exists because a failed pull once led to a re-clone that erased the
primary checkout with every local branch and worktree.

## The helpers

Every helper takes the target and the **expected parent** folder, and refuses
(deleting nothing) when the target:

- is empty, `/`, `HOME`, the passwd home, or an ancestor of either;
- is not strictly inside the expected parent;
- is a `.git` folder or git history inside one;
- has a `.git` folder (a primary checkout), or a `.git` file (a linked
  worktree) unless the caller says it is removing its own worktree;
- is or contains a protected folder: the projects root, any project folder,
  each project's `src`, `data`, `config`, `ssh`, `state`, `logs`,
  `~/projects/forkmesh-backup`, the `FORKMESH_*` data and config folders,
  `~/.ssh`, and the legacy data folders;
- sits inside a protected folder where deleting is not expected (only
  disposable folders such as `build*`, `cache`, `tmp`, `worktrees`,
  `node_modules`, `dist` inside `src`; only second-level entries inside data
  folders; nothing inside `forkmesh-backup`).

Both `HOME` and the passwd home are protected, so a test that sandboxes `HOME`
but inherits `FORKMESH_HOME` still cannot reach real folders. Symlinks are
unlinked, never followed. Paths are compared after resolving symlinks in the
deepest folder above the target that exists, so `/var/...` and
`/private/var/...` on macOS compare equal even when the target's own parent
does not exist yet (the Go helper resolved only an existing parent until
2026-10-15, so the agent work folder check in `server/sandbox.go` accepted a
folder under the node key's folder on macOS).

| Language | Helper | Own worktree | Move aside |
| --- | --- | --- | --- |
| C++ (header-only) | `forkmesh::safedelete::removeTree(target, parent, &why)` in `desktop/src/SafeDelete.h`; `refusal(...)` to check first | `Allow::OwnWorktree` | `moveAside(target, label)` |
| Python | `forkmesh_safe_delete.remove_tree(target, parent)` in `tools/forkmesh_safe_delete.py`; raises `RefusedDelete`; `remove_trees()` checks all targets first | `worktree=True` | `move_aside(...)` |
| Shell | `fm_safe_rm_tree <parent> <target>...` in `tools/forkmesh-safe-rm.sh`; returns 64 on refusal | `FM_SAFE_RM_WORKTREE=1` | `mv` to a stamped sibling |
| Go (`server/`) | `removeTreeUnder(parent, target)` in `server/safedelete.go` | `removeOwnWorktreeUnder`, `removeOwnCloneUnder` | `moveAside(target, label)` |

`www/public/install.sh` and `uninstall.sh` run as `curl | bash`, so they carry
a byte-identical copy of `fm_safe_rm_tree`. Standalone server tools whose few
deletes cannot import a helper bound them in place and are allow-listed.

## The contract test

`app/tests/test_no_unguarded_destructive_deletes.py` scans tracked sources
(tests excluded) for recursive deletes in every language used (`rm -r`,
`shutil.rmtree`, `QDir::removeRecursively`, `std::filesystem::remove_all`,
`os.RemoveAll`, `deleteRecursively`, `removeItem(at:)`, recursive `rmSync`,
`Remove-Item -Recurse`) and history-destroying git commands (`clean -f`,
`reset --hard`, `checkout -f`, forced worktree removal, `branch -D`, force or
mirror pushes, `stash drop/clear`, `gc --prune=now`, `filter-branch`).

Each hit must be in a helper file or match an `ALLOW` entry (path, line regex,
and a reason of at least four words). The test also fails on stale entries,
more than twelve entries in one file, drift between the helpers'
disposable-folder lists, and embedded shell copies that differ from the
canonical one. Helper behaviour is tested in
`app/tests/test_safe_delete_helpers.py`, `desktop/tests/test_safe_delete.cpp`
and `server/safedelete_test.go`.

To add a delete: call the helper with the tightest parent you know. Add an
allow entry only when no helper can run there (a remote shell string, a
`mktemp -d` trap, an explicit user-confirmed history rewrite), with a reason
that says why the target can never be a checkout or user data.

## Patterns that replace deletes

- Agent clones and worktrees are reused; unpushed work is never discarded.
- Forced branch moves first save the old tip under a backup ref.
- Deleting a project moves it aside as `<slug>.deleted-<stamp>`.
- Conflicting files in moves are kept as `<name>.conflict-<stamp>`.
- The in-app uninstall removes only roots that pass the wipe checks, opens a
  root that holds a checkout instead of removing it whole, and lists every
  kept folder in the confirmation.
- User-data wipes (`install.sh --uninstall/--reinstall/--purge`,
  `uninstall.sh --purge`) refuse from an agent shell or outside `HOME`.
