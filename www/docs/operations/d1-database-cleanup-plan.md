# D1 database cleanup plan

Rules for the App's SQL database (schema in `app/src/schema.py`, migrations in
`app/migrations/`). Production runs it on rqlite on the core nodes
([architecture](../architecture.md)); the code keeps the D1 interface name
([d1.md](../../../docs/app/d1.md)). The rules apply to any SQLite-compatible store
that replays the same migrations.

## Goals

- A clean replay of `app/migrations/*.sql` in version order builds every table
  the App uses.
- No table remains without a reader or writer unless a keep decision is
  recorded.
- Hot paths read shared snapshots instead of scanning tables per request.
- Read paths write nothing: mirror selection uses stateless rotation with a
  short clone pin, not a stored cursor.
- Retention prunes run from cron at most once an hour, never per request.
- No plaintext secret column remains, and sealed rows name the key that sealed
  them.

## Schema change rules

- Take the next free migration number. Each migration has a matching `down`
  file.
- Any change to the runtime schema text (`SCHEMA_STATEMENTS`,
  `SCHEMA_PRE_CREATE_ALTER_STATEMENTS`, `SCHEMA_ALTER_STATEMENTS`) moves three
  things together: the latest migration ends with the
  `schema_fingerprint:<sha256>` stamp insert, `_SCHEMA_FINGERPRINT` holds the
  same literal, and the migration test pins that file name. A schema-only
  change with no DDL still needs a stamp-only migration.
- `CREATE TRIGGER IF NOT EXISTS` never rewrites an existing trigger; a changed
  body needs `DROP TRIGGER IF EXISTS` first.
- A new cron phase is added to `CRON_PHASE_CATALOG` in
  `app/src/cron_activity.py`.
- Never key liveness on `checked_at`.

## Dropping a table

Dropping a table takes two deploys:

1. Deploy N removes every reader, writer and the table's DDL from `schema.py`
   (`ensure_schema` replays `schema.py` whenever the fingerprint changes, so
   leftover DDL re-creates the table).
2. Deploy N+1 ships the `DROP` migration, after a full export of the database.

Never put both in one change.

## Retention

| Table family | Rule |
| --- | --- |
| Metrics and status samples | pruned from cron, at most hourly |
| Rate-limit tables | pruned by age from cron |
| `sensitive_audit_log` | write agent status entries only on a real transition; shorter retention for high-volume actions is an open decision |
| `release_downloads` | daily roll-up of older rows is an open decision |
| `feedback_email_sends` | never pruned by age: a missing row means "not sent yet" |

## Sealed rows

`encrypt_row` seals JSON with AES-GCM under a key derived from `DATA_KEY`;
`decrypt_row` returns `None` on any failure. Stored values do not yet name
their key, so `DATA_KEY` cannot be rotated in place. The planned format is
`k1.<8 hex key id>:<base64>` (key id = first 8 hex of HMAC-SHA256 over the key
material with `forkmesh-data-key-id-v1`); unprefixed legacy values keep
decrypting with the current key, a previous key is kept only until every row
is re-sealed, and blind index output does not change. Sealing code should go
through `encrypt_row`/`decrypt_row` so the format applies everywhere.

## Exports and measurement

Before any destructive migration:

1. Take a fresh backup: `python3 tools/next_backup.py run --execute` keeps it
   only after `PRAGMA integrity_check` and table counts pass.
2. Check it and print its row counts:
   `python3 tools/next_backup.py verify <file> --identity <key>`.
3. Decrypt it into scratch SQLite:
   `python3 tools/next_backup.py restore <file> --out <scratch.sqlite3> --identity <key>`,
   run the migration there and compare row counts with production.
4. List what will run: `python3 tools/rqlite_migrate.py --list`, then apply
   with `python3 tools/rqlite_migrate.py --url <leader> --user <u> --password <p>`.

Measure reads with `FORKMESH_HOST_D1_LOG=1`: each statement is followed by a
`D1-done <ms> ms, <rows> rows` line ([d1.md](../../../docs/app/d1.md)).

On a [Workers self-host](worker-split.md), export with
`wrangler d1 export <db> --remote --output <file>`.

## Related

- [d1.md](../../../docs/app/d1.md)
- [Node backup and recovery](node-backup-and-recovery.md)
