-- Native mirror-node daily security scan scheduling.
--
-- Repository identities and lease capabilities are blind indexes. Commit and
-- runner metadata remain in the encrypted data blob; no local diagnostics,
-- source excerpts, credentials, or reusable bearer token are stored.

CREATE TABLE IF NOT EXISTS repo_security_scan_leases (
    repo_bi TEXT PRIMARY KEY,
    lease_token_bi TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL,
    acquired_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    next_attempt_at INTEGER NOT NULL DEFAULT 0,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    completed_scan_id TEXT NOT NULL DEFAULT '',
    updated_at INTEGER NOT NULL,
    data TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_repo_security_scan_leases_due
    ON repo_security_scan_leases(status, expires_at, next_attempt_at);
