-- ForkMesh D1 migration 0002 — server error log.
-- Backs the admin error dashboard (secret ADMIN_PATH, Basic-auth gated).
-- The worker also creates this lazily (ensure_schema in src/entry.py); this
-- migration just keeps the file-based schema in sync. Plaintext (operational
-- diagnostics only — no user content), unlike the encrypted data tables.

CREATE TABLE IF NOT EXISTS error_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,        -- epoch ms
    status INTEGER NOT NULL,    -- HTTP status (>= 500)
    method TEXT,
    path TEXT,
    message TEXT,               -- exception repr or "response status N"
    ray TEXT                    -- CF-Ray header, to cross-reference in Cloudflare
);

CREATE INDEX IF NOT EXISTS idx_error_log_ts ON error_log(ts);
