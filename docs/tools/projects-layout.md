# Projects layout

ForkMesh keeps everything for one project under one folder.
The resolver exists twice and the two must agree: `desktop/src/AppPaths.{h,cpp}`
(desktop) and `tools/forkmesh_paths.py` (every tool; `tools/forkmesh-paths.sh`
for shell scripts).

```text
~/projects/                     projects root      (FORKMESH_PROJECTS_DIR)
  forkmesh/                     the home project   (FORKMESH_HOME)
    src/                        source checkout
    bin/ toolchain/ builds/ worktrees/ release/    shared by every project
    data/                       this project's own files
      core/                     identity/, profile/, mirrors/, config/, cache/, state/, logs/, ssh/
      <plugin>/                 one folder per plugin that keeps data
    config/ ssh/ state/ logs/ cache/   links into data/core (home project only)
  <project>/                    any other project, fully isolated
    data/core/ data/<plugin>/   nothing else: project.json, the logo, src/ if it has one
```

## Plugin folders (2026-10-19)

Each project's `data/` holds one folder per plugin that keeps data, named by
the plugin's id, and `core/` for everything that belongs to no plugin: the
identity key, profile, repository mirrors, network log, settings (`config/`),
`ssh/`, `state/`, `logs/` and `cache/`. A plugin's folder appears the first
time it writes data, so a project only holds folders for the plugins it uses.

The table `pluginDataEntries()` in `desktop/src/AppPaths.h` (twin:
`PLUGIN_DATA_ENTRIES` in `tools/forkmesh_paths.py`) maps each name the code
used directly under `data/` before the change to its place now:

| Before (`data/…`) | Now (`data/…`) |
| --- | --- |
| `actions`, `agents`, `notes`, `vault`, `backups`, `rss-reader`, `secure-drive` | unchanged |
| `agent-accounts`, `agent-images`, `agent-runtime`, `agent-screenshots`, `prompt`, `teammate-workspaces` | `agents/accounts`, `agents/images`, `agents/runtime`, `agents/screenshots`, `agents/prompt`, `agents/teammate-workspaces` |
| `chat_history`, `avatars`, `local_mesh` | `chat/history`, `chat/avatars`, `chat/local-mesh` |
| `pull-meta`, `pull-review` | `pull-requests/meta`, `pull-requests/review` |
| `usage-reminders` | `usage/reminders` |
| `fleet_logs` | `logs-hosts/fleet` |
| `whisper.cpp`, `parakeet` | `speech/whisper.cpp`, `speech/parakeet` |
| anything else | `core/<name>` |

Desktop code asks for a location with `forkmesh::paths::dataPath("<old
name>")` (or `pluginDataDir("<plugin id>")` for a new plugin folder);
`dataRoot()` is `data/core`, and `dataDir()` is the whole `data/` folder
(backups, the complete-system export and account reset cover all of it).
Tools use `layout.data_path(...)`, `layout.core` and `layout.home_root(...)`.
Agent session records stay in `data/agents/sessions`, so the agents plugin's
other folders sit beside them without being read as sessions.

Removing a plugin never deletes its folder: if `data/<plugin>` holds anything,
the desktop says where it is, and the user decides. It says so in a notice
(`flashMessage`), not a dialog: the modal `QMessageBox` it used at first
blocked `setPackageInstalled` until clicked, and the offscreen window tests,
which uninstall packages, waited on it forever (2026-10-25).

The desktop moves a box at its next start, under its single-instance lock,
with renames only: `<project>/{config,cache,logs,ssh}` move into `data/core`,
the state folder's entries move beside the held lock and the lock is adopted
afterwards, and every entry of `data/` moves to its row of the table. The home
project leaves links at `~/projects/forkmesh/{config,cache,logs,ssh,state}`
for the fleet tools and systemd units; other projects keep none. Saved paths
in the settings file are repointed from the moves themselves.

A box backup that tars `~/projects/forkmesh/data/identity`,
`data/profile` or `config` by name must use `data/core/…` (or follow links)
once the box has moved.

`python3 tools/forkmesh_paths.py` prints where each root resolves on this box,
including the settings file (`data/core/config/ForkMesh/ForkMesh.conf`), fan-out hosts
(`config/fanout/hosts`), tool state (`state/tools`) and tool secrets
(`config/tools/secrets`), plus the source publisher's paths (bare mirror,
`config/fanout/gateways`, `state/publisher`) and the fleet operator paths
(`config/fleet`, `ssh/fleet`, `data/fleet`, `logs/fleet`; see
[source-publisher.md](source-publisher.md)).

## Resolution rules (`tools/forkmesh_paths.py`)

- Each root uses its place in the layout when that folder exists, or when there
  is nothing at the legacy location to preserve. Otherwise it stays on the
  legacy path: `~/.local/share/ForkMesh/ForkMesh` (data),
  `~/.config/ForkMesh` (settings), `~/.cache/ForkMesh/ForkMesh` (cache),
  `~/.forkmesh` (state and diagnostics).
- If the project folder is itself a git checkout (the old layout, source at
  `~/projects/forkmesh`), every root stays legacy so application data never
  sits where `git add -A` can reach it.
- If any `XDG_*_HOME` points away from its default and neither
  `FORKMESH_HOME` nor `FORKMESH_PROJECTS_DIR` is set, the run is treated as a
  sandbox and keeps its files under XDG.
- A process spawned by the desktop gets `FORKMESH_PROJECT`,
  `FORKMESH_DATA_DIR` and `FORKMESH_CONFIG_DIR`; for that project they win.
- `FORKMESH_PROJECT` must be a slug (`[a-z0-9-]`, up to 48 characters);
  anything else means the home project.

## Moving a box

```sh
python3 tools/forkmesh_layout.py          # plan only, prints each step
python3 tools/forkmesh_layout.py apply    # do it
forkmesh --migrate-layout --dry-run       # preview the desktop's own half
```

The tool moves the checkout to `~/projects/forkmesh/src` and repairs
worktrees, moves box-level folders (build trees, release workspace,
toolchains, tool state, secrets) into the layout with links left behind
(`--no-links` to skip), moves user systemd units to `config/systemd` (linked
back), and repoints the desktop's saved checkout. It keeps a
`.before-forkmesh-projects` copy of each file it edits and refuses to run while
the desktop, its services, Actions runs or git operations are live
(`--ignore-live` overrides). The desktop moves its own data, settings, cache,
diagnostics and SSH keys at its next start, under its single-instance lock.

## Phones

The iOS and Android apps keep the same shape, `<project>/data/core/…` and
`<project>/data/<plugin>/…`, in a data folder they pick on their own and the
user can change: see [../native/DATA_FOLDER.md](../native/DATA_FOLDER.md).

## Never erase

`data/` (with `data/core/config`, `ssh`, `state` and `logs`) and the
`config/`, `ssh/`, `state/` and `logs/` links hold the node identity key,
settings, the mirror-fleet SSH key and agent history. `AGENTS.md` forbids
deleting them; tests that run installers must sandbox `HOME` and `XDG_*_HOME`
and drop `FORKMESH_HOME` and `FORKMESH_PROJECTS_DIR` (see
`app/tests/conftest.py`). `www/public/install.sh` and
`www/public/uninstall.sh` refuse a data wipe from an agent shell or outside
`HOME`.
