-- "How are we doing?" founder feedback email, sent once per user account
-- ~24h after signup. One row per send doubles as the once-only guard and the
-- admin's send log; `name` is the public node name only.
CREATE TABLE IF NOT EXISTS feedback_email_sends (
    account_bi TEXT PRIMARY KEY, name TEXT, sent_at INTEGER NOT NULL);
