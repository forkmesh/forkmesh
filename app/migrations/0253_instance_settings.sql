CREATE TABLE IF NOT EXISTS instance_settings (
  k TEXT PRIMARY KEY,
  doc TEXT NOT NULL,
  updated_at INTEGER NOT NULL);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:ccb4a21a7b56dd70a946de99ac282799fd8632bb9ecfabf4718b5b60f3d020d3',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
