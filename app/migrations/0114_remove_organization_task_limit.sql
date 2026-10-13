-- Organization task history is durable and must not become an admission gate
-- for new human or agent work. Response sizes remain bounded independently.
DROP TRIGGER IF EXISTS trg_organization_task_limit;
