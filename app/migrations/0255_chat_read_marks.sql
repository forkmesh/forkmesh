CREATE TABLE IF NOT EXISTS chat_read_marks (
  account_bi   TEXT NOT NULL,
  conversation TEXT NOT NULL,
  read_ts      INTEGER NOT NULL DEFAULT 0 CHECK (read_ts >= 0),
  updated_at   INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0),
  PRIMARY KEY (account_bi, conversation)
);
CREATE INDEX IF NOT EXISTS idx_chat_read_marks_updated
  ON chat_read_marks(account_bi, updated_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:f2a36f8ca3409b11a29b2ede274ebd0ecb2491ce40e250a388e7312924974700',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
