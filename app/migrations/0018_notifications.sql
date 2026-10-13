-- First-class Worker notification inbox.
-- Canonical collaboration events still live in signed repo files or pending
-- inbox tables; this table is only an encrypted per-recipient index/read state.

CREATE TABLE IF NOT EXISTS notifications (
  dedupe_bi    TEXT PRIMARY KEY, -- blind_index("notification:<recipient>:<dedupe>")
  recipient_bi TEXT NOT NULL,    -- blind_index(account name)
  ts           INTEGER NOT NULL,
  read_at      INTEGER NOT NULL DEFAULT 0,
  data         TEXT NOT NULL     -- AES-GCM encrypted notification payload
);

CREATE INDEX IF NOT EXISTS idx_notifications_recipient_ts
  ON notifications(recipient_bi, ts);
CREATE INDEX IF NOT EXISTS idx_notifications_unread
  ON notifications(recipient_bi, read_at, ts);
