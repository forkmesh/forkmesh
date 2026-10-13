-- ForkMesh D1 migration 0013 — anonymous installer diagnostics.
-- Backs the admin "install diagnostics" funnel (secret ADMIN_PATH, Basic-auth
-- gated). The worker also creates this lazily (ensure_schema in src/entry.py);
-- this migration keeps the file-based schema in sync.
--
-- Privacy: this table is anonymous by design. `run` is a random id the installer
-- mints per run — it is NOT linked to any account, email, node name, or IP, and
-- the worker never records the client IP for these events. Plaintext operational
-- diagnostics only (which step succeeded/failed, coarse platform), unlike the
-- encrypted data tables.

CREATE TABLE IF NOT EXISTS install_diag (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,        -- epoch ms
    run TEXT NOT NULL,          -- anonymous random per-run id (client generated)
    step TEXT NOT NULL,         -- start|mirror|deps|fetch|build|install|launch|done
    ok INTEGER NOT NULL,        -- 1 = step succeeded, 0 = step failed
    os TEXT,                    -- uname -s (Linux/Darwin/…)
    arch TEXT,                  -- uname -m (x86_64/arm64/…)
    pm TEXT,                    -- detected package manager (apt/dnf/brew/…)
    distro TEXT,                -- /etc/os-release ID (debian/fedora/…)
    version TEXT,               -- install.sh INSTALLER_VERSION
    detail TEXT                 -- short non-identifying note (e.g. missing deps)
);

CREATE INDEX IF NOT EXISTS idx_install_diag_ts ON install_diag(ts);
CREATE INDEX IF NOT EXISTS idx_install_diag_run ON install_diag(run);
