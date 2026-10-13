# rqlite conformance findings (Phase 0 step 4)

- **Version:** rqlite `v10.3.6`, SQLite 3.53.4, go1.27.1. This is pinned in
  `app/host/RQLITE_VERSION`.
- **Binary:** `rqlite-v10.3.6-linux-amd64.tar.gz` from the GitHub release. Its
  sha256, `17df480e697e135641fc66aac6c2e7d938c021311e1a8500070e730ccdfa8aa0`,
  matched the release asset digest. The binary is installed to
  `~/projects/forkmesh/data/fleet/bin/`.
- **Tests:** `app/host/tests_rqlite/` holds the checks
  (`test_rqlite_conformance.py`, `test_d1_rqlite_adapter.py`,
  `test_rqlite_migrate.py`). They start real 1-node and 3-node clusters on
  127.0.0.1 with data under `mktemp`. They skip when `rqlited` is missing (set
  `RQLITED=/path` to use another binary). Run them with:
  `.venv/bin/python -m pytest app/host/tests_rqlite`.
- **Latest run:** 44 tests pass in about 50–80 s, depending on load.

## Summary

| Check | Result | Notes |
|---|---|---|
| RETURNING via `/db/request` | pass | `INSERT/DELETE ... RETURNING` returns `rows`. It works when sent to a follower, which forwards it to the leader. |
| `?transaction` atomicity | pass | Any error rolls back the whole request. Without `?transaction`, statements before the error stay applied. |
| `?associative` shape | pass | Each result is `{"types": {col: decltype}, "rows": [{col: val}]}`. Writes return `last_insert_id`/`rows_affected`. A value of 0 is **omitted** (Go omitempty), and an empty SELECT has `rows` absent or null. |
| `rows_affected` / `last_insert_id` | pass | These map to D1 `meta.changes` / `meta.last_row_id`. `last_insert_id` on DDL is stale junk, so ignore it. |
| JSON1 (`json_extract`, `json_each`, `json_object`) | pass | |
| Window functions | pass | `row_number`, `sum() OVER`, `lag`. |
| int64 round-trip | pass | INT64 min/max and ±2^53±1 are exact in both params and results. REAL `1.0` comes back as JSON `1`, and only `types` says it is real. The adapter converts it back to float. |
| BLOB params / results | pass, with a caveat | A JSON array of byte values binds as a BLOB. Results are base64 by default or byte arrays with `?blob_array`. **But** rqlite formats values by *declared* column type: a BLOB in an untyped or TEXT column, or from `randomblob()`, comes back as text, and invalid UTF-8 is replaced (lossy). The app stores no BLOBs today; all sealed data is TEXT. |
| UNIQUE error text | pass | `UNIQUE constraint failed: t.col`, exactly SQLite's text. The adapter reports it as `D1_ERROR: UNIQUE constraint failed: t.col: SQLITE_CONSTRAINT`, so the app's `"UNIQUE constraint failed: users.email_bi" in str(error)` still matches. |
| `ON CONFLICT DO UPDATE` (+ RETURNING) | pass | |
| Non-deterministic rewrite: `RANDOM()`, `randomblob()`, `'now'` time functions | pass (replicas agree) | These are rewritten on the leader into literals before replication, so all 3 replicas store identical values. |
| Rewrite uses **host local time** | **surprise** | The rewritten `'now'` is the host's local wall clock read as if it were UTC. On an EDT box, `strftime('%s','now')` is 4 h behind `unixepoch()`. It is harmless on UTC servers. Run core nodes with `TZ=UTC`. |
| Rewrite mangles `CURRENT_TIMESTAMP` | **surprise** | When a rewrite re-serializes a statement, `CURRENT_TIMESTAMP` becomes the string `'CURRENT_TIMESTAMP'`. The same happens when only `random()` triggers the rewrite. It needs both `norwtime&norwrandom` to be left alone. The app never uses it in SQL. |
| Not rewritten | note | `unixepoch()` (no args), `date()` and `CURRENT_*` are *not* rewritten. In a write they would run per replica. The app does not use them. `/db/query` never rewrites. |
| Trigger `trg_chat_channel_member_remove_rotate` (0069) | **fail → fixed by 0191** | See the next section. |
| `norwrandom`: replicas diverge | confirmed | With the rewrite off, `random()` differs on every replica. |
| Declared `TIMESTAMP`/`DATETIME` columns | note | rqlite returns them as RFC 3339 text (`2026-09-24T01:40:25Z`), where SQLite gives `2026-09-24 01:40:25`. Only `d1_migrations.applied_at` has that type. |
| Local read-only `sqlite3` on the rqlite file | **safe with conditions** | See the local-read section below. |

## The 0069 trigger

The body of 0069's trigger is
`updated_at = CAST(strftime('%s','now') AS INTEGER) * 1000`.

- **Created with the default rewrite** (what `wrangler`-style replay through
  `/db/request` does), rqlite re-serializes it like this:

  ```
  CREATE TRIGGER "trg_chat_channel_member_remove_rotate" AFTER DELETE ON
  "chat_channel_members" BEGIN UPDATE "chat_channels" SET "key_version" =
  "key_version" + 1, "updated_at" = CAST(strftime('%s', 2461307.398615) AS
  INTEGER) * 1000 WHERE "channel_id" = "OLD"."channel_id"; END
  ```

  The trigger text is **frozen**. Every later removal stamps the instant the
  trigger was created, shifted by the host's UTC offset. Replicas agree,
  because the frozen text is what gets replicated. `test_trigger_0069_body_is_frozen_by_the_rewrite` covers this.
- **Created with `norwtime&norwrandom`**, the text is kept verbatim. Then each
  replica evaluates `'now'` on its own clock when it applies the DELETE, and
  again whenever it replays its log. Replicas **diverge**: this was seen at ms
  precision on the local 3-node cluster
  (`test_trigger_0069_without_rewrite_evaluates_now_per_replica`).

**Fix (migration 0191):**

- The trigger now only does `key_version = key_version + 1`. That is the
  security-relevant rotation, and it stays atomic with the membership delete.
- The `updated_at` display stamp moved into the one app path that removes a
  single member, `chat_channels_api._remove_member`. After a real removal it
  runs `UPDATE chat_channels SET updated_at=MAX(updated_at, runtime.now())`.
- The other two delete paths lose only the display timestamp bump, never the
  rotation:
  - whole-channel delete, where the channel row is deleted too;
  - account deletion, `entry._delete_chat_channel_memberships`.

Moving the whole rotation to app code was rejected. It would have needed edits
at three delete sites, and it would lose the "same write" atomicity.

Other changes that went with 0191:

- `schema.py` carries the same deterministic body.
- The fingerprint contract was updated: 0191 stamps the new fingerprint,
  `entry._SCHEMA_FINGERPRINT` was updated, and `test_local_migrations` now
  names 0191. `test_trigger_0191_is_deterministic_on_every_replica` checks it.
- A down migration restores 0069's body.

## In-process read-only `sqlite3` on rqlite's file

- **Where the file lives:** rqlite v8+ is always on-disk. The live database is
  `<data dir>/db.sqlite` in WAL mode, next to `db.sqlite-wal`,
  `db.sqlite-shm`, `raft.db` and `wsnapshots/`. rqlite sets
  `wal_autocheckpoint=0` on its writer and checkpoints only when it takes a
  snapshot.
- **How we opened it:** `sqlite3.connect("file:…/db.sqlite?mode=ro",
  uri=True, isolation_level=None)` plus `PRAGMA query_only=1`.

Results:

1. **Under continuous writes plus forced snapshots** (`-raft-snap 64
   -raft-snap-int 100ms`, 600 two-table transactions, more than 500 snapshot
   indexes advanced), 3 reader threads ran with both persistent and
   per-read connections. We saw no errors, no torn reads (`count(a) ==
   count(b)` in every read), no backwards reads, and every reader caught up
   to the final value. No snapshot failed.
2. **A reader that holds a read transaction pins the WAL.** rqlite's snapshot
   checkpoint gives up after 250 ms (`failed to take snapshot: … checkpoint
   did not complete within 250ms`) and retries until the reader lets go.
   Writes keep working, but the WAL and the Raft log grow meanwhile. So local
   reads must be short autocommit statements that are fully fetched at once.
   `d1_rqlite` aborts a local read after 100 ms (a progress handler) and
   re-runs it over HTTP.
3. **Restart or snapshot restore replaces `-wal`/`-shm` but keeps
   `db.sqlite`'s inode.** This covers both a node restart and a lagging
   follower that installs a snapshot.
   - A connection opened before the swap **stays stale forever**.
   - Because SQLite shares one shm mapping per database inode *per process*,
     a brand-new connection opened while a stale one is alive is stale too.
   - Only closing every connection fixes it.
   - This is covered by
     `test_restart_replaces_wal_and_shm_so_open_readers_go_stale_forever`.
   - If a checkpoint later writes new pages into the main file under such a
     stale reader, it could also read a torn mix, so this must never happen.

**Verdict: safe with conditions.** `read_mode="local"` is safe if it is used
the way `d1_rqlite` does:

- one connection per process;
- the `(db, -wal, -shm)` inode triple checked before every read, with a
  reopen on any change;
- fall back to HTTP when a file is missing, busy, interrupted or malformed;
- never an open transaction.

`test_local_mode_survives_restart_by_reopening` covers this.

Local reads are equivalent to `level=none` on that node, so a partitioned
follower serves stale data. The same is true of HTTP `level=none` without
`freshness`. On a 3-node local cluster, point reads take about **16 µs**
locally against about **1.2 ms** over HTTP; writes are about 2–2.7 ms p50.

## Migrations (`tools/rqlite_migrate.py`)

- **Result:** all 199 files apply on a fresh 1-node and a fresh 3-node
  cluster. On tmpfs the full run takes about **2.3–2.6 s**. An earlier run on
  the same box under load average 19 took 54 s.
- **Replicas:** the resulting schemas are byte-identical across replicas,
  including trigger text.
- **Re-runs:** a second run applies nothing.
- **Failure on a fresh database, both plain SQLite and rqlite:**
  `0089_world_office_task_completion.sql` runs `ALTER TABLE
  world_office_marketing_tasks ADD COLUMN completed_at`. But
  `0075_world_office_marketing_tasks.sql` was later edited to create that
  column. The tool skips an `ADD COLUMN` whose column already exists and
  reports it. Production D1 never replays these files.
- **D1-specific files:** `0186_d1_read_guard.sql` and the others apply
  unchanged. The tool then sets `d1_read_guard.mode='off'`.
- **The rewrite rule:** it is disabled only for a file whose CREATE
  TRIGGER/VIEW body uses a non-deterministic function. That is only 0069,
  which 0191 then replaces. Everything else keeps the rewrite, so DML backfills
  agree across replicas.

## Open gaps

- Timings were measured on tmpfs (`/tmp`), where fsync is free. Measure write
  latency on a real Vultr disk in Phase 0 step 3.
- `freshness` for HTTP `level=none` reads is configurable but unset.
  Decide on a bound, and add an equivalent check for local reads, such as
  comparing the applied index from `/status`.
- There is no automated guard against new app SQL using
  `CURRENT_TIMESTAMP`/`unixepoch()`/`date()` or a `'now'` inside a trigger. A
  grep-style test over `app/src` and `app/migrations` would catch it.
- BLOB values in non-BLOB columns are lossy over HTTP (see the table). If the
  app ever stores raw bytes, it must declare the column `BLOB`.
