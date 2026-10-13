ALTER TABLE mail_sends DROP COLUMN status_at;
ALTER TABLE mail_sends DROP COLUMN status;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:5b55da66e894c5d0907472e46d05ec99696242bfbf7fbed5928660fc59e38a28';
