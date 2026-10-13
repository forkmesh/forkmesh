-- Recreates the retired founders outreach roster and send log (empty); the
-- rows dropped by the up migration are not recoverable.
CREATE TABLE IF NOT EXISTS outreach_team (
    name_bi TEXT PRIMARY KEY, name TEXT NOT NULL,
    added_by TEXT, added_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS outreach_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,
    sender TEXT NOT NULL, ok INTEGER NOT NULL DEFAULT 1,
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_outreach_log_sender_ts ON outreach_log(sender, ts);
DELETE FROM schema_meta
 WHERE k = 'schema_fingerprint:c05700d9ee07fbd9beccc8b73d964f4843859fb9731220465fa2b2fd45faf949';
