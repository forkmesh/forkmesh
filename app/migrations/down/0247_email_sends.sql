CREATE TABLE IF NOT EXISTS task_email_sends (
        send_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        task_id TEXT NOT NULL,
        recipient_bi TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL,
        sent_at INTEGER NOT NULL,
        opened_at INTEGER NOT NULL DEFAULT 0,
        clicked_at INTEGER NOT NULL DEFAULT 0,
        viewed_at INTEGER NOT NULL DEFAULT 0,
        open_count INTEGER NOT NULL DEFAULT 0,
        click_count INTEGER NOT NULL DEFAULT 0,
        view_count INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS idx_task_email_sends_recipient
    ON task_email_sends(recipient_bi, sent_at);
CREATE INDEX IF NOT EXISTS idx_task_email_sends_sent
    ON task_email_sends(sent_at);

INSERT OR IGNORE INTO task_email_sends (
  send_id, org_bi, task_id, recipient_bi, data, sent_at, opened_at,
  clicked_at, viewed_at, open_count, click_count, view_count
)
SELECT send_id,
       substr(ref, 6, length(ref) - 5 - 33),
       substr(ref, length(ref) - 31),
       recipient_bi, data, sent_at, opened_at, clicked_at, viewed_at,
       open_count, click_count, view_count
FROM email_sends
WHERE kind = 'task' AND ref LIKE 'task:%:%' AND length(ref) > 38;

DROP INDEX IF EXISTS idx_email_sends_recipient;
DROP INDEX IF EXISTS idx_email_sends_sent;
DROP TABLE IF EXISTS email_sends;
