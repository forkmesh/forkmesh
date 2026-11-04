ALTER TABLE mirror_https_endpoints ADD COLUMN source_ip TEXT NOT NULL DEFAULT '';

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:4af287707e32223803b7af9a0f97fb901c47f2d95cb5543c4e70addbe1a7a687',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
