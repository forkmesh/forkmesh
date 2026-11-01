CREATE TABLE IF NOT EXISTS stale_pull_repos (
  repo_bi TEXT PRIMARY KEY,
  next_at INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL,
  data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_stale_pull_repos_due ON stale_pull_repos(next_at);

CREATE TABLE IF NOT EXISTS stale_pulls (
  repo_bi TEXT NOT NULL,
  number INTEGER NOT NULL,
  anchor_at INTEGER NOT NULL DEFAULT 0,
  stage INTEGER NOT NULL DEFAULT 0,
  stage_at INTEGER NOT NULL DEFAULT 0,
  version INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL,
  data TEXT NOT NULL,
  PRIMARY KEY (repo_bi, number));
CREATE INDEX IF NOT EXISTS idx_stale_pulls_age ON stale_pulls(updated_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:1fedcc6e32c5ad36228b7a434ebca84ce5f431fbaf6146d18d7a1e48a74137b4',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
