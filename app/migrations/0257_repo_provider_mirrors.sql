CREATE TABLE IF NOT EXISTS repo_provider_mirrors (
  repo_bi TEXT PRIMARY KEY,
  provider TEXT NOT NULL,
  active INTEGER NOT NULL DEFAULT 1,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  next_sync_at INTEGER NOT NULL DEFAULT 0,
  last_sync_at INTEGER NOT NULL DEFAULT 0,
  data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_repo_provider_mirrors_due
  ON repo_provider_mirrors(active, next_sync_at);

CREATE TABLE IF NOT EXISTS repo_provider_mirror_links (
  repo_bi TEXT NOT NULL,
  kind TEXT NOT NULL,
  local_key TEXT NOT NULL,
  remote_key TEXT NOT NULL,
  origin TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  PRIMARY KEY (repo_bi, kind, remote_key));
CREATE INDEX IF NOT EXISTS idx_repo_provider_mirror_links_local
  ON repo_provider_mirror_links(repo_bi, kind, local_key);

CREATE TABLE IF NOT EXISTS repo_provider_mirror_outbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  repo_bi TEXT NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  next_ts INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_repo_provider_mirror_outbox_due
  ON repo_provider_mirror_outbox(next_ts);
CREATE INDEX IF NOT EXISTS idx_repo_provider_mirror_outbox_repo
  ON repo_provider_mirror_outbox(repo_bi);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:44afdc37ea7e2a6e51da34ebec1ae83071f63f35990738c4c0f71afa5253716d',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
