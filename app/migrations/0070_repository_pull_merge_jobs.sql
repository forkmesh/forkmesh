-- Idempotent account-authorized pull merge jobs. The Worker retains only
-- blind indexes, exact public Git object ids in a bounded result, and the
-- independently operated node selected for execution. Repository bytes and node
-- credentials never enter D1. Expiry and database triggers enforce seven-day
-- application retention plus hard per-repository/global record ceilings.

CREATE TABLE IF NOT EXISTS repo_merge_jobs (
    request_id TEXT PRIMARY KEY,
    request_digest TEXT NOT NULL,
    repo_bi TEXT NOT NULL,
    actor_bi TEXT NOT NULL,
    pull_number INTEGER NOT NULL,
    selected_node TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN (
        'requested','succeeded','failed')),
    result TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL CHECK (expires_at > created_at)
);
CREATE INDEX IF NOT EXISTS idx_repo_merge_jobs_repo
    ON repo_merge_jobs(repo_bi, status, updated_at);
CREATE INDEX IF NOT EXISTS idx_repo_merge_jobs_expiry
    ON repo_merge_jobs(expires_at);
CREATE TRIGGER IF NOT EXISTS trg_repo_merge_jobs_repo_bound
BEFORE INSERT ON repo_merge_jobs
WHEN (SELECT COUNT(*) FROM repo_merge_jobs
       WHERE repo_bi=NEW.repo_bi) >= 256
BEGIN
    SELECT RAISE(ABORT, 'repo_merge_jobs_repo_limit');
END;
CREATE TRIGGER IF NOT EXISTS trg_repo_merge_jobs_global_bound
BEFORE INSERT ON repo_merge_jobs
WHEN (SELECT COUNT(*) FROM repo_merge_jobs) >= 10000
BEGIN
    SELECT RAISE(ABORT, 'repo_merge_jobs_global_limit');
END;
