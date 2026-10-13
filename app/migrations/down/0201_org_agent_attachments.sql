-- Reverse 0201_org_agent_attachments.sql: drop the attachment store and the
-- jobs-by-session index. Any stored images are lost with it.
DROP INDEX IF EXISTS idx_org_agent_attachments_created;
DROP TABLE IF EXISTS org_agent_attachments;
DROP INDEX IF EXISTS idx_org_agent_jobs_session;
