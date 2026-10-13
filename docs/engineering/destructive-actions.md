# Destructive actions

## The rule

No code in this repository deletes a folder tree, or runs a git command that
throws away commits, branches, stashes or uncommitted work, except through a
guarded helper, or from a line on the contract test's allow-list with a
written reason.

A failure that may be transient (a network error, a 5xx, a timeout, a failed
`git pull`, `fetch` or `clone`) never leads to a delete, a `reset --hard` or a
re-clone of an existing checkout. The code stops, says what failed, and leaves
everything as it was. When an existing folder really has to make room, it is
moved aside to a timestamped sibling (`<name>.replaced-YYYYmmdd-HHMMSS`), never
deleted. A clone only goes into a folder that is missing or empty.

## Why

On 2026-09-29 the desktop's **Update, rebuild & restart** ran
`git pull --ff-only` against core node n3, which answered 503. The update
treated every pull failure as "re-clone": it ran `rm -rf ~/projects/forkmesh/src`
and erased the primary checkout, its `.git` (every local branch) and every
agent worktree under it. The pull was fixed in commit a6c9187686
(`app/tests/test_desktop_update_keeps_checkout.py`). This page, the helpers and
the contract test make the whole class of bug hard to reintroduce.

The same shape was in `www/public/install.sh`: a failed pull fell into
`clean_clone`, which removed the installer-managed checkout, and the retry
after a failed build removed it again.

## The helpers

Every helper takes the target and the **expected parent** folder the caller
believes the target lives in, and refuses (deletes nothing) when the target:

- is empty, `/`, `HOME`, the account's passwd home, or an ancestor of either;
- is not strictly inside the expected parent, or the parent is empty or `/`;
- is a `.git` folder, or git history inside one (`.git/objects`, `refs`,
  `logs`, `HEAD`, `packed-refs`);
- has a `.git` folder (a primary checkout);
- has a `.git` file (a linked worktree), unless the caller says it is removing
  a worktree it created (`Allow::OwnWorktree`, `worktree=True`,
  `FM_SAFE_RM_WORKTREE=1`, `removeOwnWorktreeUnder`). A primary checkout is
  refused even then;
- is, or contains, a protected folder: the projects root (`~/projects`,
  `FORKMESH_PROJECTS_DIR`), any project folder in it, each project's
  `src`, `data`, `config`, `ssh`, `state`, `logs`, `~/projects/forkmesh-backup`,
  `FORKMESH_HOME`, `FORKMESH_PROJECT_DIR`, `FORKMESH_DATA_DIR`,
  `FORKMESH_CONFIG_DIR`, `~/.ssh`, and the legacy `~/.forkmesh`,
  `~/.config/ForkMesh`, `~/.local/share/ForkMesh`, `~/.local/share/forkmesh`;
- sits inside a protected folder where deleting is not expected:
  - inside `src` or `ssh`, only paths under a disposable folder (`build`,
    `build-*`, `cache`, `.cache`, `tmp`, `worktrees`, `node_modules`, `dist`,
    `out`, and a few more) may go;
  - inside `data`, `config`, `state`, `logs` and the legacy data folders, the
    top-level entries (`data/identity`, `data/mirrors`) are refused, while
    anything one level further down (`data/mirrors/<repo>`) may go;
  - inside a project folder, only its shared cache folders (`builds`,
    `worktrees`, `cache`, `bin`, `toolchain`, `release`, `tmp`) or disposable
    folders (and `toolchain-*`) may go;
  - inside a checkout's tracked `.forkmesh/` metadata (issues, discussions,
    pulls), anything below `.forkmesh` itself may go: it is committed and
    recoverable from git;
  - nothing inside `forkmesh-backup` may go.

Both the process `HOME` and the passwd home are protected, so a test that
sandboxes `HOME` but inherits `FORKMESH_HOME` (the 2026-09-22 incident) still
cannot reach the real folders.

| Language | Helper | Worktree form | Move aside |
| --- | --- | --- | --- |
| C++ (desktop, header-only) | `forkmesh::safedelete::removeTree(target, parent, &why)` in `desktop/src/SafeDelete.h`; `refusal(...)` to check first | `Allow::OwnWorktree` | `moveAside(target, label)` |
| Python | `forkmesh_safe_delete.remove_tree(target, parent)` in `tools/forkmesh_safe_delete.py`; raises `RefusedDelete`; `refusal()` / `check()` to test first; `remove_trees()` checks every target before deleting any | `worktree=True` | `move_aside(target)` |
| Shell | `fm_safe_rm_tree <parent> <target>...` in `tools/forkmesh-safe-rm.sh`; returns 64 and prints `Refusing to delete: ...` | `FM_SAFE_RM_WORKTREE=1` | `mv` to a stamped sibling |
| Go (`server/`) | `removeTreeUnder(parent, target)` in `server/safedelete.go` | `removeOwnWorktreeUnder`; `removeOwnCloneUnder` for a full clone the node made itself | `moveAside(target, label)`; `asideName(dir, name, label, now)` picks a free `<name>.<label>-<stamp>[-n]` |

`SafeDelete.h` is header-only so every store compiled into a standalone test
binary can use it without CMake changes; it reads the active projects layout
through the inline accessors in `AppPaths.h`. A symlink target is unlinked,
never followed. The C++ `removeTree` returns true when the target is gone
(including "was never there"), false when refused or not fully removed.

`www/public/install.sh` and `uninstall.sh` run as `curl | bash`, so they carry
a byte-identical copy of `fm_safe_rm_tree`; the contract test compares every
copy with `tools/forkmesh-safe-rm.sh`. Tools shipped standalone to mirror and
gateway servers (`mirror_gateway.py`, `node_operator_policy.py`,
`ssh_gateway.py`, `secret_gate.py`) cannot import new modules without changing
every bundle list, so their few deletes are bounded in place and allow-listed.

## The contract test

`app/tests/test_no_unguarded_destructive_deletes.py` scans the tracked sources
in `desktop/`, `app/`, `world/`, `tools/`, `deploy/`, `www/public/`,
`marketplace/`, `native/`, `server/` and `conformance/` (tests excluded) for:

- `rm -r…` in shell text and in argv lists (`"rm", "-rf"`, `QStringList{"-rf"}`);
- `shutil.rmtree`, `QDir::removeRecursively`, `std::filesystem::remove_all`,
  `os.RemoveAll`;
- Kotlin `deleteRecursively`, Swift `removeItem(at:)`, Dart
  `delete(recursive: true)`, Node `rmSync(..., {recursive: true})`, `rimraf`,
  PowerShell `Remove-Item -Recurse`;
- `git clean -f`, `reset --hard`, `checkout -f`, `worktree remove --force`,
  `branch -D`, `push --force`/`--mirror`/`+refs`, `stash drop/clear`,
  `gc --prune=now`, `filter-branch`.

Each hit must be in a helper file or match an `ALLOW` entry: the file path, a
regex for the line, and a reason of at least four words. The test also fails
on stale allow entries, on more than twelve entries in one file (write a helper
instead), when the helpers' disposable-folder lists drift apart, and when an
embedded shell copy differs from the canonical one. Helper behaviour is tested
in `app/tests/test_safe_delete_helpers.py` (Python and shell),
`desktop/tests/test_safe_delete.cpp` (`forkmesh-safe-delete-tests`) and
`server/safedelete_test.go`.

To add a delete: call the helper with the tightest parent you know. Only when
no helper can run there (a remote shell string, a `mktemp -d` trap in a build
script, an explicit user-confirmed history rewrite), add an allow entry whose
reason says why the target can never be a checkout or user data.

The **Scrub a term** shortcut (`.forkmesh/shortcuts/scrub-term.sh`, run from
the desktop's search page) is such a user-confirmed rewrite: the term is typed
again before it runs, every ref is saved to a backup bundle first, it refuses
a checkout with uncommitted changes, and it leaves `refs/stash` and its reflog
alone. `.forkmesh/` is outside the contract test's scan, so it needs no allow
entry ([saved searches](../desktop/saved-searches.md#scrub-a-term-2026-10-13)).

## Sites found in the 2026-09-29 audit

High-risk sites, where a checkout, a worktree with uncommitted work, or user
data could be lost. All are fixed:

| Site | What could happen | Now |
| --- | --- | --- |
| `www/public/install.sh` `fetch_source` / `clean_clone` | A failed `git pull --ff-only` (a 503) re-cloned and `rm -rf`'d the managed `~/projects/forkmesh/src`; an incomplete or foreign checkout was "replaced"; the retry after a failed build deleted it again | A failed pull tries the next mirror with a pull, then stops, changing nothing. A foreign checkout stops the install. An incomplete managed checkout is moved to `$SRC.replaced-<stamp>`. A clone only goes into a missing or empty folder |
| `www/public/install.sh --uninstall/--reinstall`, `uninstall.sh` | Removed the source checkout (and, without `--purge`, `~/.local/share/forkmesh`, the legacy checkout's parent) | The checkout is kept, with a message; non-purge deletes use `fm_safe_rm_tree` |
| `desktop/src/MainWindowSetup.cpp` update | The 2026-09-29 incident (fixed in a6c9187686); the remaining `rm -rf` steps (stale CMake entries, `.forkmesh-tmp`) | Every operand is checked with `safedelete::refusal` against the build folder first; a refusal stops the update |
| `desktop/src/AgentRunner.cpp` `releaseBranchWorktree`, `MainWindowInternal4.cpp` and `PullStore.cpp` `releaseWorktreeHoldingBranch` | `worktree remove --force` plus a recursive delete of any worktree holding a branch: a user's own worktree, an agent's dirty worktree, or the primary checkout under another spelling | Only worktrees under the agent worktree root are touched, with plain `git worktree remove` (dirty trees survive) |
| `desktop/src/MainWindowAgentLanding.cpp` `cleanupStreamWorktree` | The stream "worktree" can be the repository itself | Removed only when proven inside an agent root |
| `desktop/src/MainWindowAccounts.cpp` uninstall | Deleted `FORKMESH_SOURCE_DIR` (the checkout's `desktop/`) and `~/.local/share/forkmesh` (the legacy installer checkout) | Source folder dropped; `uninstallMayErase()` skips HOME, checkouts and project containers. See [The in-app uninstall](#the-in-app-uninstall) |
| `desktop/src/MainWindowData.cpp` Delete all data | Iterated every storage-map location, including the project folder holding `src/` and other projects | Only deletable, non-link folders that pass `userDataWipeRefusal()` |
| `desktop/src/MainWindowRepoSettings.cpp` Delete repository | Could erase a registered local checkout | `removeTree` refuses a checkout and says why |
| `desktop/src/ProjectRegistry.cpp` delete project | Erased `~/projects/<slug>` with its identity key, ssh keys and repositories | Moved aside to `<slug>.deleted-<stamp>` |
| `desktop/src/MainWindowSourceControl.cpp` discard untracked folder | Raw recursive delete, including nested repositories | `git clean -fd -- <path>` on the SCM worker; git keeps nested repositories |
| `desktop/src/UpstreamCheckoutSync.cpp` automatic sync | `reset --hard`, `branch --force` and `branch -D` of pull branches a lagging mirror stopped advertising (the transient-failure pattern) | Every forced move or prune first saves the old tip under `refs/forkmesh/trash/sync/` |
| `desktop/src/PullStore.cpp` conflict-merge abort | `checkout -B` reset an existing local head branch and abort deleted it | Old tip saved to `refs/forkmesh/am-backup/`, restored on abort; `-D` only for a branch the flow created |
| `desktop/src/MainWindowBranches.cpp` merge rollback, `keepResolvedLocalEdits` | `reset --hard` with local edits not in an autostash; dropped the newest stash by name, possibly another session's | Rollback needs the autostash; the name-based drop is gone |
| `desktop/src/MainWindowRepoDetail.cpp` delete commit, reassign attribution | `reflog expire --all` + `gc --prune=now` on the whole repository destroyed recovery for unrelated work | Repo-wide prune removed |
| `desktop/install.sh --purge` | Purge list includes the legacy installer checkout; no agent guard | Refuses in an agent shell, keeps folders outside HOME or holding a checkout |
| `tools/hosted_repository_sync.py` cleanup | A missing sidecar file deleted every imported server mirror | A missing or malformed sidecar removes nothing |
| `tools/package/linux-baseline/oci_rootfs.py` whiteouts | A layer symlink could make a whiteout delete outside the rootfs | `remove_tree(path, rootfs)` |
| `tools/forkmesh-release-host.py` Windows script | Empty `$build` gave `rm -rf /install` | `${FORKMESH_WIN_BUILD_DIR:?}` |
| `tools/android-toolchain.sh` | Empty toolchain dir gave `rm -rf /_jdk_build` | `fm_safe_rm_tree` with the toolchain parent |
| `server/intake.go` issue delete and move | `RemoveAll(filepath.Dir(existingPath))` unbounded | `removeTreeUnder(<repo>/.forkmesh/issues, ...)` |
| `server/agents.go` `prepareAgentWorkspace` (now `server/agent_workspace.go`) | Every agent job deleted and re-cloned the session's clone, losing commits and edits a previous job had not pushed | The clone is reused; an unpushed `HEAD`, the local branch and a dirty tree are saved under `refs/forkmesh/trash/agent/<session>/` first (newest 20 kept), and the forced checkout and `clean -fd` run only after the save succeeded. A broken clone is moved to `<dir>.replaced-<stamp>` ([agent-jobs](../server/agent-jobs.md#the-session-clone)) |
| `server/intake.go` `moveIssueDirectory` | Deleted source entries it skipped because the name was taken at the destination | Kept as `<name>.conflict-<stamp>[-n]` in the destination and logged; nothing is deleted |
| Desktop project picker (`MainWindowProjectPicker.cpp`) | Said "Delete project" although the folder is only moved aside | "Remove project…"; the dialog and the notice say the folder is kept as `<slug>.deleted-<stamp>` with its identity key, settings and data, and the notice gives the real path |

Everything else (scratch and `mktemp` folders, run and cache folders, bare
mirror temp dirs, own scratch worktrees, remote fleet shell strings, sandboxed
iOS/Android app containers, explicit user-confirmed history purges) either goes
through a helper now or is on the allow-list with its reason. The full
allow-list is the `ALLOW` table in the contract test.

The agent clone and the issue move, open after the audit, were decided on
2026-09-29 and are in the table above. Still open:

- `MainWindowData.cpp` and `MainWindowAccounts.cpp` build their wipe checks on
  `safedelete::detail`; a public `userDataWipeRefusal()` in `SafeDelete.h`
  would be tidier.
- `server/safedelete.go` has `removeOwnCloneUnder` (a full clone the node made
  itself, `.git` folder allowed); the C++ and Python helpers have no such form.

## The in-app uninstall

**Settings → Uninstall ForkMesh** (`MainWindow::uninstallForkMesh`) promises to
erase all ForkMesh data on the computer, and on 2026-10-09 it was made to keep
that promise. Before, it left data behind silently:

- a folder with even one git checkout directly inside it was dropped whole,
  so a data root holding one cloned repository kept the identity key, chat and
  settings;
- with the projects layout active it skipped the legacy XDG folders
  (`~/.local/share/ForkMesh`, `~/.config/ForkMesh`, `~/.cache/ForkMesh`), which
  can still hold an old identity key or a compatibility link;
- a mirror root, browse cache or backup folder moved out of the data root in
  settings was never listed;
- the file-manager entries (`Open with ForkMesh`) were left.

Now each owned root is checked with `uninstallMayErase()` (not HOME, not a
checkout, not a projects or project folder, holds no protected root). A root
that has a checkout directly inside it (`uninstallHoldsCheckout()`) is opened
instead: every other entry in it is removed, and the checkout stays. A
symbolic link is removed as a link, never followed. A mirror, browse or backup
root set in settings may be any folder of the user's, so it is removed only
whole and never opened. The confirmation lists every folder removed and, under
**Kept**, every checkout or folder it will not touch, such as the legacy
installer checkout `~/.local/share/forkmesh/src`. Each removed path still
passes through the same `sleep 1; rm -rf` entry on the contract test's
allow-list. Not touched: the source checkout, other projects, the shared
`bin`, `builds`, `toolchain*`, `release` and `worktrees` folders of the home
project, and the user's systemd units, which are operator tools rather than
app data.
