DROP INDEX IF EXISTS idx_organization_tasks_reminder_due;

ALTER TABLE organization_tasks DROP COLUMN reminder_next_at;

ALTER TABLE organization_tasks DROP COLUMN reminder_every_ms;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:11bb61205cb9c0b05b1e2ffd2eba0ba60776f0d0f8b05c03744a63ed3177d90f';
