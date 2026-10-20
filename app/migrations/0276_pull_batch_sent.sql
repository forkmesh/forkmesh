CREATE TABLE IF NOT EXISTS pull_batch_sent (
    repo_bi TEXT NOT NULL,
    target_bi TEXT NOT NULL,
    branch TEXT NOT NULL,
    head TEXT NOT NULL,
    number INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, target_bi, branch, head)
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:ea85061f75e5602fc9c86c7d07a8c15583c2cdc95ead6a0463968bfd0031400c',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
