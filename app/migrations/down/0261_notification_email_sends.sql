DROP INDEX IF EXISTS idx_notification_email_sends_created;
DROP TABLE IF EXISTS notification_email_sends;
ALTER TABLE notifications DROP COLUMN content_bi;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:4e9c64d2bf5a897f1aa75a4e0456bcb7f77f83d68b43af86165cfc9e6f65cc0c';
