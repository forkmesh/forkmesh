ALTER TABLE notifications ADD COLUMN content_bi TEXT NOT NULL DEFAULT '';

CREATE TABLE IF NOT EXISTS notification_email_sends (send_bi TEXT PRIMARY KEY, created_at INTEGER NOT NULL CHECK (created_at >= 0));

CREATE INDEX IF NOT EXISTS idx_notification_email_sends_created ON notification_email_sends(created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:4e9c64d2bf5a897f1aa75a4e0456bcb7f77f83d68b43af86165cfc9e6f65cc0c',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
