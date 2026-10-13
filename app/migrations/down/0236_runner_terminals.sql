DELETE FROM runner_login_chunks WHERE session_id IN (
  SELECT id FROM runner_login_sessions WHERE kind='terminal');

DELETE FROM runner_login_sessions WHERE kind='terminal';

DROP INDEX IF EXISTS idx_runner_login_sessions_open;

CREATE UNIQUE INDEX IF NOT EXISTS idx_runner_login_sessions_active
  ON runner_login_sessions(runner, provider)
  WHERE state IN ('pending','running');

ALTER TABLE runner_availability DROP COLUMN features;

ALTER TABLE runner_login_sessions DROP COLUMN term_rows;

ALTER TABLE runner_login_sessions DROP COLUMN term_cols;

ALTER TABLE runner_login_sessions DROP COLUMN kind;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:7d4343fa69339b8e4e2d24eaf06fdb35f0bf203119a30a57642df88ed9f60a8f';
