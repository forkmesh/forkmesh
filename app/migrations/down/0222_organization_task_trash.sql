DROP INDEX IF EXISTS idx_organization_tasks_trash;

ALTER TABLE organization_tasks DROP COLUMN deleted_by_bi;

ALTER TABLE organization_tasks DROP COLUMN deleted_at;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:48ef712408116a4c4a4a9ffe5ec9d3906b339ccbfcfd84c81aa783d28a3ef102';
