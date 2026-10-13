CREATE TABLE IF NOT EXISTS email_sends (
        send_id TEXT PRIMARY KEY,
        kind TEXT NOT NULL,
        recipient_bi TEXT NOT NULL DEFAULT '',
        ref TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL,
        sent_at INTEGER NOT NULL,
        opened_at INTEGER NOT NULL DEFAULT 0,
        clicked_at INTEGER NOT NULL DEFAULT 0,
        viewed_at INTEGER NOT NULL DEFAULT 0,
        open_count INTEGER NOT NULL DEFAULT 0,
        click_count INTEGER NOT NULL DEFAULT 0,
        view_count INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS idx_email_sends_recipient
    ON email_sends(recipient_bi, sent_at);
CREATE INDEX IF NOT EXISTS idx_email_sends_sent
    ON email_sends(sent_at);

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

INSERT OR IGNORE INTO email_sends (
  send_id, kind, recipient_bi, ref, data, sent_at, opened_at, clicked_at,
  viewed_at, open_count, click_count, view_count
)
SELECT send_id, 'task', recipient_bi, 'task:' || org_bi || ':' || task_id,
       data, sent_at, opened_at, clicked_at, viewed_at, open_count,
       click_count, view_count
FROM task_email_sends;

DROP INDEX IF EXISTS idx_task_email_sends_recipient;
DROP INDEX IF EXISTS idx_task_email_sends_sent;
DROP TABLE task_email_sends;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:6fad7832f96066e4e9d1dd3ce5726b03ecc0599e78557afc5278ee3da3ccf727',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
