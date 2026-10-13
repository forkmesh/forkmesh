ALTER TABLE runner_login_sessions
       ADD COLUMN kind TEXT NOT NULL DEFAULT 'login'
       CHECK (kind IN ('login','terminal'));

ALTER TABLE runner_login_sessions
       ADD COLUMN term_cols INTEGER NOT NULL DEFAULT 0;

ALTER TABLE runner_login_sessions
       ADD COLUMN term_rows INTEGER NOT NULL DEFAULT 0;

ALTER TABLE runner_availability
       ADD COLUMN features TEXT NOT NULL DEFAULT '';

DROP INDEX IF EXISTS idx_runner_login_sessions_active;

CREATE UNIQUE INDEX IF NOT EXISTS idx_runner_login_sessions_open
  ON runner_login_sessions(runner, provider, kind)
  WHERE state IN ('pending','running');

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:7d4343fa69339b8e4e2d24eaf06fdb35f0bf203119a30a57642df88ed9f60a8f',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
