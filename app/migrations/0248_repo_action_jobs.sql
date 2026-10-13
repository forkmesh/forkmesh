CREATE TABLE IF NOT EXISTS repo_action_jobs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        branch TEXT NOT NULL,
        commit_oid TEXT NOT NULL DEFAULT '',
        target_node TEXT NOT NULL DEFAULT '',
        pusher TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL DEFAULT 'queued',
        lease_id TEXT NOT NULL DEFAULT '',
        runner TEXT NOT NULL DEFAULT '',
        attempts INTEGER NOT NULL DEFAULT 0,
        expires_at INTEGER NOT NULL DEFAULT 0,
        note TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_repo_action_jobs_repo
    ON repo_action_jobs(owner, repo, id);
CREATE INDEX IF NOT EXISTS idx_repo_action_jobs_status
    ON repo_action_jobs(status, id);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:871f73bd739ff6ceddd9d84cfc5b1b76c8c7f36210843fc58a22e0fe91e206a7',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
