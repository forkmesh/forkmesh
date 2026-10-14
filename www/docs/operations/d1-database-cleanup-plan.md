# D1 database cleanup plan

This page records the rules that came out of auditing the App's SQL database
(schema in `app/src/schema.py`, migrations in `app/migrations/`). It applies to
the Cloudflare D1 deployment and to any SQLite-compatible store that replays the
same migrations.

## Goals

- A clean replay of `app/migrations/*.sql` in version order builds every table
  the App uses.
- No table remains without a reader or writer unless a keep decision is
  recorded.
- Hot paths read shared snapshots instead of scanning tables per request.
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

Before any destructive migration, export the database
(`wrangler d1 export <db> --remote --output <file>`), load it into scratch
SQLite and compare row counts with production. Measure reads with
`wrangler d1 insights <db> --time-period 7d --sort-by reads`; table-browser
`COUNT(*)` queries come from the dashboard, not App code.

## Related

- `docs/app/d1.md`
- [Cloudflare free-tier audit](cloudflare-free-tier-audit.md)
