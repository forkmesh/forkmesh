CREATE TABLE IF NOT EXISTS mail_budget (
    bucket TEXT PRIMARY KEY,
    used INTEGER NOT NULL DEFAULT 0,
    stamp_ms INTEGER NOT NULL DEFAULT 0
);
ALTER TABLE security_incidents ADD COLUMN email_state TEXT;
