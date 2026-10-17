CREATE TABLE IF NOT EXISTS repo_feature_settings (
        repo_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
        updated_at INTEGER NOT NULL);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:83509a81a8e93dafa25a304813ccf5c2565ec5a74b82d932773e741acc242305',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
