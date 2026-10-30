CREATE TABLE IF NOT EXISTS repo_mirror_quota (
    repo_bi TEXT PRIMARY KEY,
    target INTEGER NOT NULL DEFAULT 0 CHECK (target >= 0),
    next_check_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL,
    data TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_repo_mirror_quota_due ON repo_mirror_quota(next_check_at);
CREATE TABLE IF NOT EXISTS repo_mirror_registry (
    repo_bi TEXT NOT NULL,
    mirror_id TEXT NOT NULL,
    added_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    next_check_at INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    PRIMARY KEY (repo_bi, mirror_id)
);
CREATE INDEX IF NOT EXISTS idx_repo_mirror_registry_due ON repo_mirror_registry(next_check_at);
CREATE INDEX IF NOT EXISTS idx_repo_mirror_registry_adder ON repo_mirror_registry(added_by_bi, created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:3a826662aff96ff870fdb057b5cedb5c57e52adc867aff9b4d7ca8f538891cbc',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
