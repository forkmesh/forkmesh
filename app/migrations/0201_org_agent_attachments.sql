CREATE INDEX IF NOT EXISTS idx_org_agent_jobs_session
ON org_agent_jobs(session_id, status);

CREATE TABLE IF NOT EXISTS org_agent_attachments (
    session_id TEXT NOT NULL,
    attachment_id TEXT NOT NULL,
    chunk_index INTEGER NOT NULL,
    org_bi TEXT NOT NULL,
    repo TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (session_id, attachment_id, chunk_index)
);

CREATE INDEX IF NOT EXISTS idx_org_agent_attachments_created
ON org_agent_attachments(created_at);
