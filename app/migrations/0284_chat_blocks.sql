CREATE TABLE IF NOT EXISTS chat_blocks (
    account_bi TEXT PRIMARY KEY,
    data TEXT NOT NULL,
    updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0)
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:7ea1fe7cb13e1cb599a8c5f6ede34f1f947311c2e34608c648ae4389817ee147',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
