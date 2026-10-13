# Rules for AI agents working in this repository

## Never erase ForkMesh user data

These folders hold the node identity key (lose it and the account cannot be
recovered), settings, the mirror-fleet SSH key, and agent history:

- `~/projects/forkmesh/{data,config,ssh,state,logs}`
- legacy paths: `~/.config/ForkMesh`, `~/.local/share/ForkMesh`, `~/.forkmesh`
- `~/projects/forkmesh/src`, the primary checkout: its `.git` holds every local
  branch and every agent worktree (erased by the desktop's update on
  2026-09-29, `docs/desktop/README.md`)

Do **not** run anything that deletes them — ask the user to do it themselves:

- `www/public/install.sh --uninstall`, `--reinstall` or `--purge`
- `www/public/uninstall.sh --purge`
- `rm -rf` / `shutil.rmtree` on any of the paths above

Both scripts now refuse a data wipe from an agent shell or outside `HOME`, but
that guard is a backstop, not permission.

## Tests that run the installer scripts

On 2026-09-22 a test sandboxed only `HOME`, inherited `FORKMESH_HOME` (the
desktop exports it to every agent it launches), and a reinstall erased the real
`~/projects/forkmesh` data. A test subprocess must:

- set `HOME` **and** `XDG_DATA_HOME` / `XDG_CONFIG_HOME` / `XDG_CACHE_HOME` to a temp dir;
- drop `FORKMESH_HOME` and `FORKMESH_PROJECTS_DIR` from its environment
  (`app/tests/conftest.py` and `world/tests/conftest.py` already do this).

Before any recursive delete of a path built from an environment variable,
check the path is inside a temp directory.

## Destructive actions go through the guarded helpers

Never write a raw recursive delete or history-destroying git command. Use
`forkmesh::safedelete::removeTree` (C++), `forkmesh_safe_delete.remove_tree`
(Python), `fm_safe_rm_tree` (shell) or `removeTreeUnder` (Go), each with the
expected parent folder. A failed pull, fetch, clone or HTTP call must never
fall back to deleting or re-cloning a checkout: stop and report, or move it
aside. `app/tests/test_no_unguarded_destructive_deletes.py` enforces this; see
`docs/engineering/destructive-actions.md`.

## The served stack (ForkMesh Next)

forkmesh.com, app.forkmesh.com and world.forkmesh.com run the stack that was
built as ForkMesh Next: three self-hosted core nodes and a runner tier, no
Cloudflare Workers, D1 or Durable Objects. `app/host/`, `deploy/`,
`tools/next_*.py` and `tools/cutover_*.py` drive it; `docs/PLAN.md` and
`docs/NEXT_ADOPTION.md` describe it.

- Vultr instances are labelled `fmnext-*` and tagged `forkmesh-next`. Never add
  them to the desktop's saved hosts.
- Its operator files live in `~/projects/forkmesh/{data,config,ssh,logs}/fleet`
  (moved 2026-09-24, `docs/NEXT_ADOPTION.md`); the never-erase rule applies.
  `~/projects/forkmesh-backup` is the retired Next folder, kept as a backup:
  never erase it either.

## Source is code only

Source files hold code; human-readable text lives in `docs/`. Do not add
comments or docstrings. Put the reason for a change in the matching
`docs/*.md`; `docs/README.md` maps each subsystem to its home page. The only exceptions are lines tools read: shebangs, encoding
lines, build tags and `//go:embed`, lint/type directives, license headers,
build markers in templates, and user-facing help or UI text.
