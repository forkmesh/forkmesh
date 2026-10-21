CREATE TABLE IF NOT EXISTS recent_pushes (
    account_bi TEXT NOT NULL,
    repo_bi TEXT NOT NULL,
    branch_bi TEXT NOT NULL,
    pull_bi TEXT NOT NULL,
    pushed_at INTEGER NOT NULL,
    pulled INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    PRIMARY KEY (account_bi, repo_bi, branch_bi)
);
CREATE INDEX IF NOT EXISTS idx_recent_pushes_account ON recent_pushes(account_bi, pushed_at);
CREATE INDEX IF NOT EXISTS idx_recent_pushes_branch ON recent_pushes(repo_bi, branch_bi);
CREATE INDEX IF NOT EXISTS idx_recent_pushes_pull ON recent_pushes(pull_bi);
CREATE INDEX IF NOT EXISTS idx_recent_pushes_age ON recent_pushes(pushed_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:809f8ba0db32b4ca902b82f56c42fc89ccf03eb50dbf54e3b7e509c9cbc1e5da',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
