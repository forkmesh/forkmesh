# rqlite conformance findings

The App's D1-compatible database is rqlite. This page records how rqlite
behaves where it differs from plain SQLite, and the rules the host adapter
(`app/host/forkmesh_host/d1_rqlite.py`) and the App follow because of it.

- **Version:** pinned in `app/host/RQLITE_VERSION`; the release tarball is
  verified against its published sha256 and installed under
  `~/projects/forkmesh/data/fleet/bin/`.
- **Tests:** `app/host/tests_rqlite/` (`test_rqlite_conformance.py`,
  `test_d1_rqlite_adapter.py`, `test_rqlite_migrate.py`) start real 1-node and
  3-node clusters on 127.0.0.1 with data under a temp dir. They skip when
  `rqlited` is missing (`RQLITED=/path` selects another binary). Run with
  `.venv/bin/python -m pytest app/host/tests_rqlite`.

## Behavior

| Check | Result | Notes |
|---|---|---|
| RETURNING via `/db/request` | pass | Returns `rows`; a follower forwards it to the leader. |
| `?transaction` atomicity | pass | Any error rolls back the whole request; without it, earlier statements stay applied. |
| `?associative` shape | pass | `{"types": {col: decltype}, "rows": [{col: val}]}`. A zero `last_insert_id`/`rows_affected` is omitted; an empty SELECT has `rows` absent or null. |
| `rows_affected` / `last_insert_id` | pass | Map to D1 `meta.changes` / `meta.last_row_id`. Ignore `last_insert_id` after DDL. |
| JSON1, window functions, `ON CONFLICT DO UPDATE` | pass | |
| int64 round-trip | pass | Exact at INT64 min/max and ±2^53±1. REAL `1.0` returns as JSON `1`; the adapter restores the float from `types`. |
| BLOB values | caveat | Values are formatted by declared column type: a BLOB in an untyped or TEXT column comes back as lossy text. Raw bytes need a column declared `BLOB`. |
| UNIQUE error text | pass | Same text as SQLite; the adapter reports `D1_ERROR: UNIQUE constraint failed: t.col: SQLITE_CONSTRAINT`. |
| `RANDOM()`, `randomblob()`, `'now'` | pass | Rewritten to literals on the leader, so replicas agree. |
| `'now'` rewrite uses host local time | surprise | The literal is local wall-clock time read as UTC. Core nodes run with `TZ=UTC`. |
| `CURRENT_TIMESTAMP` under rewrite | surprise | Becomes the string `'CURRENT_TIMESTAMP'`. The App never uses it. |
| Not rewritten | note | `unixepoch()` (no args), `date()`, `CURRENT_*` run per replica in a write; `/db/query` never rewrites. The App does not use them in writes. |
| `norwrandom` | note | With the rewrite off, `random()` differs per replica. |
| `TIMESTAMP`/`DATETIME` columns | note | Returned as RFC 3339 text rather than SQLite's `YYYY-MM-DD HH:MM:SS`. |

## Triggers must be deterministic

A trigger body that calls `'now'` either has its text frozen at creation time
(with the rewrite on, every later firing stamps the creation instant) or
diverges across replicas (with the rewrite off, each replica evaluates its own
clock). Trigger and view bodies therefore use no non-deterministic function.
The chat-channel member-removal trigger only increments `key_version`, which
stays atomic with the membership delete; the `updated_at` display stamp is set
by the App in `chat_channels_api._remove_member`. `schema.py` carries the same
body and the schema fingerprint covers it.

## Local read-only access to the rqlite file

rqlite keeps the live database at `<data dir>/db.sqlite` in WAL mode
(`wal_autocheckpoint=0`; it checkpoints only when snapshotting). The adapter's
`read_mode="local"` opens it with
`sqlite3.connect("file:…/db.sqlite?mode=ro", uri=True, isolation_level=None)`
and `PRAGMA query_only=1`.

- Under continuous writes and forced snapshots, concurrent readers saw no
  errors, torn reads or backwards reads.
- A reader holding a read transaction pins the WAL: rqlite's snapshot
  checkpoint gives up after 250 ms and retries, so the WAL and Raft log grow.
  Local reads must be short autocommit statements, fully fetched; the adapter
  aborts a local read after 100 ms and re-runs it over HTTP.
- A restart or snapshot restore replaces `-wal`/`-shm` but keeps the
  `db.sqlite` inode. Every connection open across the swap stays stale, and so
  does any new connection opened in the same process while a stale one lives.

Local reads are safe only when used as the adapter does: one connection per
process; the `(db, -wal, -shm)` inode triple checked before every read with a
reopen on change; fallback to HTTP when a file is missing, busy, interrupted or
malformed; never an open transaction. Local reads are equivalent to
`level=none`, so a partitioned follower serves stale data.

## Migrations (`tools/rqlite_migrate.py`)

- Applies every file in `app/migrations` to a fresh 1-node or 3-node cluster;
  resulting schemas are byte-identical across replicas, and a re-run applies
  nothing.
- An `ADD COLUMN` whose column already exists is skipped and reported.
- After applying D1-specific files it sets `d1_read_guard.mode='off'`.
- The rewrite is disabled only for a file whose CREATE TRIGGER/VIEW body uses
  a non-deterministic function; everything else keeps it, so DML backfills
  agree across replicas.

## Open items

- `freshness` for HTTP `level=none` reads is unset; local reads have no
  equivalent applied-index check.
- No automated guard yet rejects new App SQL that uses
  `CURRENT_TIMESTAMP`, `unixepoch()`, `date()` or `'now'` inside a trigger.
