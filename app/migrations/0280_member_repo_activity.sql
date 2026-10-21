CREATE TABLE IF NOT EXISTS member_repo_activity (
    repo_bi TEXT NOT NULL,
    user_bi TEXT NOT NULL,
    commits INTEGER NOT NULL DEFAULT 0,
    pulls INTEGER NOT NULL DEFAULT 0,
    issues INTEGER NOT NULL DEFAULT 0,
    reviews INTEGER NOT NULL DEFAULT 0,
    day_start TEXT NOT NULL DEFAULT '',
    day_mask TEXT NOT NULL DEFAULT '',
    last_day TEXT NOT NULL DEFAULT '',
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, user_bi)
);
CREATE INDEX IF NOT EXISTS idx_member_repo_activity_user ON member_repo_activity(user_bi);
CREATE TABLE IF NOT EXISTS member_activity_repos (
    repo_bi TEXT PRIMARY KEY,
    refreshed_at INTEGER NOT NULL DEFAULT 0,
    people INTEGER NOT NULL DEFAULT 0,
    ok INTEGER NOT NULL DEFAULT 0
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:cf2a17988f55b1615b1581612cc4e9e9d77b984fc78923d7b5fd9da84697312d',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
