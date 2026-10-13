-- Reverse 0180. Only the task timeline is removed; tasks, replies, check-ins
-- and QA reviews are untouched.
DROP TRIGGER IF EXISTS trg_organization_task_event_limit;
DROP INDEX IF EXISTS idx_organization_task_events_task;
DROP TABLE IF EXISTS organization_task_events;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:d4d41ff733a3985d69c9a39a4f366e68030e8ec1b7990dd362251cd4995c34a2';
