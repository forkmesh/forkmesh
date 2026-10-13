-- Forward repair for installations that applied an early 0070 draft before
-- the expires_at lease column and hard bounds were finalized. Rebuild from the
-- common columns so this is safe for both that draft and the final 0070 shape.
-- Merge jobs contain no repository bytes or credentials; their seven-day lease
-- is conservatively restarted from the last recorded update.

DROP TRIGGER IF EXISTS trg_repo_merge_jobs_repo_bound;
DROP TRIGGER IF EXISTS trg_repo_merge_jobs_global_bound;
DROP INDEX IF EXISTS idx_repo_merge_jobs_repo;
DROP INDEX IF EXISTS idx_repo_merge_jobs_expiry;

ALTER TABLE repo_merge_jobs RENAME TO repo_merge_jobs_legacy_0071;

CREATE TABLE repo_merge_jobs (
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

INSERT INTO repo_merge_jobs (
    request_id, request_digest, repo_bi, actor_bi, pull_number,
    selected_node, status, result, created_at, updated_at, expires_at
)
SELECT
    request_id, request_digest, repo_bi, actor_bi, pull_number,
    selected_node, status, result, created_at, updated_at,
    CASE
        WHEN updated_at > created_at THEN updated_at + 604800000
        ELSE created_at + 604800000
    END
FROM repo_merge_jobs_legacy_0071
ORDER BY updated_at DESC
LIMIT 10000;

DROP TABLE repo_merge_jobs_legacy_0071;

CREATE INDEX idx_repo_merge_jobs_repo
    ON repo_merge_jobs(repo_bi, status, updated_at);
CREATE INDEX idx_repo_merge_jobs_expiry
    ON repo_merge_jobs(expires_at);
CREATE TRIGGER trg_repo_merge_jobs_repo_bound
BEFORE INSERT ON repo_merge_jobs
WHEN (SELECT COUNT(*) FROM repo_merge_jobs
       WHERE repo_bi=NEW.repo_bi) >= 256
BEGIN
    SELECT RAISE(ABORT, 'repo_merge_jobs_repo_limit');
END;
CREATE TRIGGER trg_repo_merge_jobs_global_bound
BEFORE INSERT ON repo_merge_jobs
WHEN (SELECT COUNT(*) FROM repo_merge_jobs) >= 10000
BEGIN
    SELECT RAISE(ABORT, 'repo_merge_jobs_global_limit');
END;
