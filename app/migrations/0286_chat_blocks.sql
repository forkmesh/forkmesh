CREATE TABLE IF NOT EXISTS chat_blocks (
    account_bi TEXT PRIMARY KEY,
    data TEXT NOT NULL,
    updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0)
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:a2baca528d73942f6a58a0dccba59b0121d5ae9b3a1b786bf72d63fec59447b4',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
