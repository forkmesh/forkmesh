-- Discord bug-channel watcher: the per-channel scan cursor and the record of
-- every message already answered.
--
-- The channel and message ids are stored only as blind indexes, so neither the
-- watched channel nor any individual report can be recovered from a copy of
-- this database. discord_bug_reports.data is an encrypted row holding the
-- verdict, the issue number the report became, and its title -- the corpus the
-- duplicate reviewer compares against while every repository host is offline.
CREATE TABLE IF NOT EXISTS discord_bug_watch (
    channel_bi TEXT PRIMARY KEY,
    last_message_id TEXT NOT NULL,
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0));

CREATE TABLE IF NOT EXISTS discord_bug_reports (
    message_bi TEXT PRIMARY KEY,
    repo_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0));

CREATE INDEX IF NOT EXISTS idx_discord_bug_reports_repo
    ON discord_bug_reports(repo_bi, created_at DESC);

-- Stamp the schema fingerprint this migration brings the database to, so cold
-- isolates take the schema_meta fast path instead of replaying every DDL
-- statement on their first request.
INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:1640d82a0041fd647352182d0fed58a7c95c8497eab98ec5daadb3578a28d15d',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
