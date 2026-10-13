-- Server-revocable, non-replayable-at-rest account sessions.
CREATE TABLE IF NOT EXISTS account_sessions (
    session_id TEXT PRIMARY KEY,
    account_bi TEXT NOT NULL,
    token_digest TEXT NOT NULL UNIQUE,
    created_at INTEGER NOT NULL,
    last_seen_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    revoked_at INTEGER NOT NULL DEFAULT 0,
    device_label TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_account_sessions_account
    ON account_sessions(account_bi, revoked_at, expires_at);
CREATE INDEX IF NOT EXISTS idx_account_sessions_expiry
    ON account_sessions(expires_at, revoked_at);
