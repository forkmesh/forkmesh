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
    data/ config/ cache/ state/ logs/ ssh/         this project's own files
  <project>/                    any other project, fully isolated
    data/ config/ cache/ state/ logs/ ssh/
```

`python3 tools/forkmesh_paths.py` prints where each root resolves on this box,
including the settings file (`config/ForkMesh/ForkMesh.conf`), fan-out hosts
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

## Never erase

`data/`, `config/`, `ssh/`, `state/` and `logs/` hold the node identity key,
settings, the mirror-fleet SSH key and agent history. `AGENTS.md` forbids
deleting them; tests that run installers must sandbox `HOME` and `XDG_*_HOME`
and drop `FORKMESH_HOME` and `FORKMESH_PROJECTS_DIR` (see
`app/tests/conftest.py`). `www/public/install.sh` and
`www/public/uninstall.sh` refuse a data wipe from an agent shell or outside
`HOME`.
