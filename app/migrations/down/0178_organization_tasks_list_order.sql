-- Drops the task-list ordering index; no data is affected.
DROP INDEX IF EXISTS idx_organization_tasks_list_order;
DELETE FROM schema_meta
 WHERE k = 'schema_fingerprint:76856bf024d6dab5f4ffd3fd7978bca030a2cc1c4d0eba228c60f9c9c50d87f0';
