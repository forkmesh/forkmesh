DROP INDEX IF EXISTS idx_runner_account_requests_runner;
DROP TABLE IF EXISTS runner_account_requests;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:14342a074189c5e6876d7e475cc0d7f1fdad578dc771aa89bad753770e3f5f42';
