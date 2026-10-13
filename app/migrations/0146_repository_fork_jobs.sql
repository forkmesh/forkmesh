-- Website-created forks are durable jobs for a linked managed mirror. The
-- target node may be offline when the user clicks Fork; it reads the encrypted
-- payload with its existing signed node credential and acknowledges only after
-- its local bare repository and gateway configuration are safely written.
CREATE TABLE IF NOT EXISTS repository_fork_jobs (
    request_id TEXT PRIMARY KEY,
    target_node_bi TEXT NOT NULL,
    target_repo_bi TEXT NOT NULL UNIQUE,
    source_repo_bi TEXT NOT NULL,
    actor_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending'
        CHECK (status IN ('pending','completed')),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    completed_at INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_repository_fork_jobs_target
    ON repository_fork_jobs(target_node_bi, status, created_at);
CREATE INDEX IF NOT EXISTS idx_repository_fork_jobs_actor
    ON repository_fork_jobs(actor_bi, status, created_at);
CREATE INDEX IF NOT EXISTS idx_repository_fork_jobs_source
    ON repository_fork_jobs(source_repo_bi, status, created_at);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:4fd8916adc179bace0db2bb14ef8d6d14a8cf9808154c5432f20464bbff93ee1',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
