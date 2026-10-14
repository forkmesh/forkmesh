# Upstream

This repository was seeded from the earlier ForkMesh repository and keeps its
own history. Fixes from the earlier repository are ported by hand.

## Where this repository differs

- `app/host/`, `deploy/`, `tools/next_*.py`, `tools/rehost.py`,
  `tools/rqlite_migrate.py` and `tools/cutover_*.py` exist only here.
- Hostnames in `app/`, `world/`, `www/`, `server/`, `tools/`, `marketplace/`
  and `conformance/` were rewritten by `tools/rehost.py`; `desktop/` and
  `native/` were not.
- A migration ported from the earlier repository takes the next free number
  (see [docs/CUTOVER.md](docs/CUTOVER.md)).
- The developer docs index is [docs/README.md](docs/README.md).
