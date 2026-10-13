-- Web sign-in of a runner's provider CLI (runner_logins.py, docs/RUNNERS.md).
--
-- runner_login_sessions one relayed `claude auth login` / `codex login
--                       --device-auth` terminal on one runner; at most one
--                       pending|running session per runner and provider.
-- runner_login_chunks   the terminal bytes in flight: 'out' (runner -> browser,
--                       only the most recent chunks are kept) and 'in' (the
--                       pasted code, deleted once the runner has read it).
--                       Every chunk of a session is deleted shortly after it
--                       ends; the credential itself never leaves the runner.
-- runner_availability   the provider availability snapshot each runner sends
--                       with every claim/poll.

CREATE TABLE IF NOT EXISTS runner_login_sessions (
        id TEXT PRIMARY KEY,
        runner TEXT NOT NULL,
        provider TEXT NOT NULL,
        state TEXT NOT NULL DEFAULT 'pending'
            CHECK (state IN ('pending','running','succeeded','failed',
                             'expired','cancelled')),
        created_by TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        ended_at INTEGER NOT NULL DEFAULT 0,
        output_seq INTEGER NOT NULL DEFAULT 0,
        input_seq INTEGER NOT NULL DEFAULT 0,
        exit_code INTEGER,
        message TEXT NOT NULL DEFAULT '');

CREATE UNIQUE INDEX IF NOT EXISTS idx_runner_login_sessions_active
ON runner_login_sessions(runner, provider)
WHERE state IN ('pending','running');

CREATE INDEX IF NOT EXISTS idx_runner_login_sessions_runner
ON runner_login_sessions(runner, state, expires_at);

CREATE TABLE IF NOT EXISTS runner_login_chunks (
        session_id TEXT NOT NULL,
        direction TEXT NOT NULL CHECK (direction IN ('out','in')),
        seq INTEGER NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (session_id, direction, seq));

CREATE TABLE IF NOT EXISTS runner_availability (
        runner TEXT PRIMARY KEY,
        data TEXT NOT NULL DEFAULT '{}',
        updated_at INTEGER NOT NULL);

-- The runtime schema text changed with it, so stamp the new fingerprint:
INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:91f2f67ed83f2439fcef33793249c0dcc1a21d8f635764ee13f26e81d4afd77f',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
