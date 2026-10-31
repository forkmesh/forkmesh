-- D1 audit (#682) repairs.
--
-- commit_inbox has had no reader or writer since 0009 and no runtime DDL;
-- forkmesh_d1_migrations is the pre-wrangler migration tracker. Both are
-- dropped (a nightly backup holds their last rows).
--
-- The tables below were created only by app/src/schema.py at runtime, so a
-- database replayed from migrations alone lacked them. They are created here
-- exactly as schema.py creates them.
--
-- The legacy system_status_minute/hourly/daily tables and host_presence lost
-- their last readers and their runtime DDL in this change; they are dropped by
-- a later migration, one deploy after this one.

DROP TABLE IF EXISTS commit_inbox;
DROP TABLE IF EXISTS forkmesh_d1_migrations;

CREATE TABLE IF NOT EXISTS agent_prompts (
    id INTEGER PRIMARY KEY AUTOINCREMENT, repo_bi TEXT NOT NULL,
    agent_id TEXT NOT NULL, data TEXT NOT NULL, queued_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_agent_prompts_repo ON agent_prompts(repo_bi);

CREATE TABLE IF NOT EXISTS ai_ask_rate (
    account_bi TEXT PRIMARY KEY, count INTEGER NOT NULL DEFAULT 0,
    window_start_ts INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS catalog_rev (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    rev INTEGER NOT NULL DEFAULT 0,
    ts INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS clone_sticky (
    repo_bi TEXT PRIMARY KEY, owner TEXT NOT NULL, ts INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS issue_seq (
    repo_bi TEXT PRIMARY KEY, next_number INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS login_attempts (
    id_bi TEXT PRIMARY KEY, fails INTEGER NOT NULL DEFAULT 0,
    first_fail_ts INTEGER NOT NULL DEFAULT 0,
    locked_until INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS online_hourly_nodes (
    hour_ts INTEGER NOT NULL, node_key TEXT NOT NULL, label TEXT NOT NULL,
    node_minutes INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (hour_ts, node_key)
);
CREATE INDEX IF NOT EXISTS idx_online_hourly_nodes_key
    ON online_hourly_nodes(node_key);

CREATE TABLE IF NOT EXISTS relay_self (
    id INTEGER PRIMARY KEY, data TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS repo_agents (
    repo_bi TEXT NOT NULL, agent_id TEXT NOT NULL,
    data TEXT NOT NULL, updated_at INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, agent_id)
);
CREATE INDEX IF NOT EXISTS idx_repo_agents_repo ON repo_agents(repo_bi);

CREATE TABLE IF NOT EXISTS repo_deletions (
    repo_bi TEXT PRIMARY KEY, owner_bi TEXT NOT NULL,
    deleted_at INTEGER NOT NULL, expires_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_repo_deletions_expires
    ON repo_deletions(expires_at);

CREATE TABLE IF NOT EXISTS repository_monitor_state (
    monitor_id TEXT PRIMARY KEY,
    is_up INTEGER NOT NULL DEFAULT 1,
    changed_at INTEGER NOT NULL,
    outage_started_at INTEGER NOT NULL DEFAULT 0,
    checked_at INTEGER NOT NULL,
    reason TEXT,
    notified_state TEXT NOT NULL DEFAULT '',
    pinged_state TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS signup_rate (
    ip_bi TEXT PRIMARY KEY, count INTEGER NOT NULL DEFAULT 0,
    window_start_ts INTEGER NOT NULL DEFAULT 0
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:f98c955c26168b8c22df7e6675d0984c66b0fce8a506edcbd698e8c0f48ccc1a',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
