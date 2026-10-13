-- Signed phone-to-desktop requests (app/src/desktop_commands.py,
-- docs/app/desktop-commands.md). data is encrypted; sig_digest is the SHA-256
-- of the request signature and refuses replays; rows older than a day are
-- deleted on the next queue write. Not part of the Worker's runtime schema
-- text, so the schema fingerprint is unchanged.

CREATE TABLE IF NOT EXISTS desktop_commands (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        repo_bi TEXT NOT NULL,
        sig_digest TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        queued_at INTEGER NOT NULL,
        done_at INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS idx_desktop_commands_repo
        ON desktop_commands(repo_bi, done_at, queued_at);
