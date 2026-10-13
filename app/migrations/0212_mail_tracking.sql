CREATE TABLE IF NOT EXISTS mail_sends (
    send_id TEXT PRIMARY KEY,
    message_id TEXT NOT NULL,
    kind TEXT NOT NULL DEFAULT '',
    account_bi TEXT NOT NULL DEFAULT '',
    recipient_bi TEXT NOT NULL DEFAULT '',
    sent_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_mail_sends_message ON mail_sends(message_id);
CREATE INDEX IF NOT EXISTS idx_mail_sends_sent ON mail_sends(sent_at);
CREATE TABLE IF NOT EXISTS mail_suppressions (
    recipient_bi TEXT PRIMARY KEY,
    reason TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
