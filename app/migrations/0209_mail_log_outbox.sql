CREATE TABLE IF NOT EXISTS mail_log_outbox (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        created_at INTEGER NOT NULL,
        next_at INTEGER NOT NULL,
        attempts INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_mail_log_outbox_due
        ON mail_log_outbox(next_at, id);
