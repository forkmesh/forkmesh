ALTER TABLE not_found_paths ADD COLUMN last_client TEXT NOT NULL DEFAULT '';

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:ce90a3db6ad2f1b24c309bcbc50b13b890c3cc605770fd4c6ae5a6a84c9cfb67',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
