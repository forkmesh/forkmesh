# Projects layout

ForkMesh keeps everything for one project under one folder.
The resolver exists twice and the two must agree: `desktop/src/AppPaths.{h,cpp}`
(desktop) and `tools/forkmesh_paths.py` (every tool; `tools/forkmesh-paths.sh`
for shell scripts).

```text
~/projects/                     projects root      (FORKMESH_PROJECTS_DIR)
  forkmesh_data/                every project's own files, in one folder
    forkmesh/                   data/ config/ cache/ state/ logs/ ssh/
    <project>/                  data/ config/ cache/ state/ logs/ ssh/
  forkmesh/                     the home project   (FORKMESH_HOME)
    src/                        source checkout
    bin/ toolchain/ builds/ worktrees/ release/    shared by every project
    project.json  logo.png
  <project>/                    any other project
    project.json  logo.png      plus folders a plugin needs (src/ for its repositories)
```

All of a project's data lives in `forkmesh_data/<project>/`; the project
folder holds only `project.json`, its logo and the folders an enabled plugin
creates. `forkmesh_data` sits beside the home project
(`$(dirname FORKMESH_HOME)/forkmesh_data`), so a run that places
`FORKMESH_HOME` in a sandbox keeps its data there too. Its name has an
underscore, which no project slug can, so it never shows up as a project.

`python3 tools/forkmesh_paths.py` prints where each root resolves on this box,
including the settings file (`config/ForkMesh/ForkMesh.conf`), fan-out hosts
(`config/fanout/hosts`), tool state (`state/tools`) and tool secrets
(`config/tools/secrets`), plus the source publisher's paths (bare mirror,
`config/fanout/gateways`, `state/publisher`) and the fleet operator paths
(`config/fleet`, `ssh/fleet`, `data/fleet`, `logs/fleet`; see
[source-publisher.md](source-publisher.md)).

## Resolution rules (`tools/forkmesh_paths.py`)

- Each root uses `forkmesh_data/<project>/<root>` when that exists. Otherwise
  it uses `~/projects/<project>/<root>` while that is still a real folder (a
  box not yet moved), then the legacy location if it holds something, and
  `forkmesh_data` for a fresh install. The state root stays in the project
  folder while its single-instance lock is there, so a second start cannot
  take a lock in the new place. The legacy paths are
  `~/.local/share/ForkMesh/ForkMesh` (data),
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

## Moving data into forkmesh_data

Each project's desktop moves its own folders at its next start, under that
project's single-instance lock (rename only, so it is instant):

- `~/projects/<project>/{data,config,cache,logs,ssh}` move to
  `forkmesh_data/<project>/`. The home project leaves a link at each old path,
  because systemd units, fleet tools and backup scripts still name
  `~/projects/forkmesh/config/...`; other projects leave nothing behind.
- `state/` cannot move while its lock is held. Everything in it but the lock
  moves; then the desktop takes the lock in `forkmesh_data/<project>/state`,
  releases and deletes the old one, removes the emptied folder and (home
  project only) links it.
- Saved settings that name the old folders are rewritten
  (`ForkMesh.conf.before-path-repoint` keeps the original).
- `forkmesh --migrate-layout --dry-run [--project <name>]` previews it.

Creating a project makes `~/projects/<slug>/` (project.json, logo) and
`forkmesh_data/<slug>/` (the roots, the seed settings, a copy of the identity
key). Removing one moves both aside as `<name>.deleted-<stamp>`.

A backup script that tars `~/projects/forkmesh` must follow the links
(`tar -h`) or archive `~/projects/forkmesh_data` itself: plain `tar` stores the
links, not the data.

## Phones

The iOS and Android apps keep a copy of their data in the same shape,
`forkmesh_data/<project>/{data,config,…}`, inside a folder the user picks, so
it survives an uninstall: see [../native/DATA_FOLDER.md](../native/DATA_FOLDER.md).

## Never erase

`forkmesh_data/` (and in it each project's `data/`, `config/`, `ssh/`,
`state/` and `logs/`) holds the node identity key,
settings, the mirror-fleet SSH key and agent history. `AGENTS.md` forbids
deleting them; tests that run installers must sandbox `HOME` and `XDG_*_HOME`
and drop `FORKMESH_HOME` and `FORKMESH_PROJECTS_DIR` (see
`app/tests/conftest.py`). `www/public/install.sh` and
`www/public/uninstall.sh` refuse a data wipe from an agent shell or outside
`HOME`.
