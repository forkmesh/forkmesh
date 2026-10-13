-- Organization-scoped Claude/Codex sessions dispatched to eligible mirrors.
-- This channel is separate from owner-only repo_agents E2EE records.
CREATE TABLE IF NOT EXISTS org_agent_sessions (
    session_id TEXT PRIMARY KEY,
    org_bi TEXT NOT NULL,
    repo TEXT NOT NULL,
    target_node TEXT NOT NULL,
    provider TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'security_pending',
    created_by_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    completed_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_org_agent_sessions_scope
ON org_agent_sessions(org_bi, repo, updated_at DESC);

CREATE TABLE IF NOT EXISTS org_agent_jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    org_bi TEXT NOT NULL,
    target_node TEXT NOT NULL,
    repo TEXT NOT NULL,
    provider TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'queued',
    lease_id TEXT NOT NULL DEFAULT '',
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_org_agent_jobs_drain
ON org_agent_jobs(target_node, repo, status, id);

-- Codex may move an explicitly linked build-board item to Done only after its
-- leased mirror job reports a terminal successful result.
ALTER TABLE world_build_board_items
ADD COLUMN completed_at INTEGER NOT NULL DEFAULT 0
CHECK (completed_at >= 0);

ALTER TABLE world_build_board_items
ADD COLUMN completed_by_bi TEXT NOT NULL DEFAULT '';
