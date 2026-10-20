CREATE TABLE IF NOT EXISTS branch_delete_jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_bi TEXT NOT NULL,
    state TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    lease_until INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_branch_delete_jobs_repo
    ON branch_delete_jobs(repo_bi, id);

CREATE INDEX IF NOT EXISTS idx_branch_delete_jobs_state
    ON branch_delete_jobs(state, lease_until);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:5ffdfea001d97bb56881a9bbee7a9d9fe108b171f3fc0b48e889c5e07041914a',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
