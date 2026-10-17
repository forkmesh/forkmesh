ALTER TABLE repo_action_jobs ADD COLUMN event TEXT NOT NULL DEFAULT 'push';

ALTER TABLE repo_action_jobs ADD COLUMN workflow TEXT NOT NULL DEFAULT '';

ALTER TABLE repo_action_jobs ADD COLUMN ref TEXT NOT NULL DEFAULT '';

ALTER TABLE repo_action_jobs ADD COLUMN actor TEXT NOT NULL DEFAULT '';

ALTER TABLE repo_action_jobs ADD COLUMN before_oid TEXT NOT NULL DEFAULT '';

ALTER TABLE repo_action_jobs ADD COLUMN payload TEXT NOT NULL DEFAULT '';

ALTER TABLE repo_action_jobs ADD COLUMN token_hash TEXT NOT NULL DEFAULT '';

ALTER TABLE repo_action_jobs ADD COLUMN trusted INTEGER NOT NULL DEFAULT 0;

ALTER TABLE repo_action_jobs ADD COLUMN pushed_commit TEXT NOT NULL DEFAULT '';

ALTER TABLE runner_nodes ADD COLUMN actions_trusted INTEGER NOT NULL DEFAULT 0;

CREATE INDEX IF NOT EXISTS idx_repo_action_jobs_token
    ON repo_action_jobs(token_hash) WHERE token_hash != '';

CREATE TABLE IF NOT EXISTS repo_action_workflows (
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        path TEXT NOT NULL,
        name TEXT NOT NULL DEFAULT '',
        events TEXT NOT NULL DEFAULT '[]',
        schedules TEXT NOT NULL DEFAULT '[]',
        inputs TEXT NOT NULL DEFAULT '{}',
        commit_oid TEXT NOT NULL DEFAULT '',
        last_slot INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (owner, repo, path));

CREATE TABLE IF NOT EXISTS repo_action_secrets (
        owner TEXT NOT NULL,
        repo TEXT NOT NULL DEFAULT '',
        name TEXT NOT NULL,
        value TEXT NOT NULL,
        updated_by TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (owner, repo, name));

CREATE TABLE IF NOT EXISTS repo_action_logs (
        job_id INTEGER NOT NULL,
        run_index INTEGER NOT NULL,
        data TEXT NOT NULL,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (job_id, run_index));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:bd23d448215d983ffe1f306a258fa0c4918fb74df8b099c19f7632986f37a6b5',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
