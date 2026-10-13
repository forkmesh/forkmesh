-- Redacted automated security-scan artifacts.
--
-- Repository identity is a keyed blind index and the documented rich +
-- clipboard envelope is AES-GCM encrypted in ``data``.  This keeps private
-- repository names, paths, and findings out of plaintext D1 storage.  The
-- Worker enforces a fixed per-repository history limit after every upsert.

CREATE TABLE IF NOT EXISTS repo_security_scans (
    repo_bi TEXT NOT NULL,
    scan_id TEXT NOT NULL,
    scanned_at INTEGER NOT NULL,
    received_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (repo_bi, scan_id)
);
CREATE INDEX IF NOT EXISTS idx_repo_security_scans_latest
    ON repo_security_scans(repo_bi, scanned_at DESC, received_at DESC);
