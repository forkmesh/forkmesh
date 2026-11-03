CREATE TABLE IF NOT EXISTS retired_account_names (
  name_bi TEXT PRIMARY KEY,
  successor_bi TEXT NOT NULL DEFAULT '',
  retired_at INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_retired_account_names_successor ON retired_account_names(successor_bi);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:57a0dddcc8a868059c8f9dcba09ec67d14ea7886b21002d334756e9cae9d0071',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
