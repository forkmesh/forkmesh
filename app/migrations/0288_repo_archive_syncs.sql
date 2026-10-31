CREATE TABLE IF NOT EXISTS repo_archive_syncs (
  repo_bi TEXT PRIMARY KEY,
  active INTEGER NOT NULL DEFAULT 1,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  data TEXT NOT NULL);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:4a97a7c9f133e3faf47c1d4b57a01f652fb5a550daf61b386207e3f0e1f16246',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
