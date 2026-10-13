CREATE TABLE IF NOT EXISTS org_pull_rules (
        org_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
        updated_by TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:5cd37ba5cc726273e4c930d597e107a00b8409ecbd47268c84fbdd6ce77efba3',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
