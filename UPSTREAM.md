# Upstream

This repository was seeded on 2026-09-23 from the earlier ForkMesh repository
with `git archive` of `main` at commit
`c48046db27132039419e888c95dcf5b311db0995`.

Directories carried over: `app/`, `world/`, `www/`, `server/`, `tools/`,
`marketplace/`, `conformance/`, `docs/`, and `desktop/` (reference only).
Left out at seed time: `mobile/`, `.forkmesh/`, `.github/`. `native/` and
`docs/native/` were imported later from upstream `87faa9064` (2026-09-24);
native app development now happens here.

This repository has its own history. Fixes from upstream are ported by hand
with a cherry-pick.

## Ported from upstream

| Next commit | Upstream commit | What |
| --- | --- | --- |
| `afa0c30` | `a8815d8c4` | Agent runs report the provider CLI's real error, once (`server/agent_errors.go`). |

## Where this repository differs from upstream

- `app/host/`, `deploy/`, `tools/next_*.py`, `tools/rehost.py`,
  `tools/rqlite_migrate.py`, `tools/cutover_*.py` exist only here.
- Hostnames were rewritten by `tools/rehost.py` in `app/`, `world/`, `www/`,
  `server/`, `tools/`, `marketplace/` and `conformance/`; `desktop/` and
  `native/` were not rewritten.
- Migrations 0191 to 0200 are this repository's own; a migration ported from
  upstream takes the next free number (see [docs/CUTOVER.md](docs/CUTOVER.md)).
- The developer docs index is [docs/README.md](docs/README.md).
