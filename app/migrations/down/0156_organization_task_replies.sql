DROP TRIGGER IF EXISTS trg_organization_task_response_limit;
DROP INDEX IF EXISTS idx_organization_task_responses_task;
DROP TABLE IF EXISTS organization_task_responses;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:8b4f4bb498e3b59b30ab3bc1c41af11badf4fdc6d159d0b52bc5defdfa39d2f8';
