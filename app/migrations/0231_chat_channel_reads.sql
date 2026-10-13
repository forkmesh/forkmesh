CREATE TABLE IF NOT EXISTS chat_channel_reads (
  channel_id  TEXT NOT NULL,
  account_bi  TEXT NOT NULL,
  read_at     INTEGER NOT NULL DEFAULT 0 CHECK (read_at >= 0),
  reminded_at INTEGER NOT NULL DEFAULT 0 CHECK (reminded_at >= 0),
  PRIMARY KEY (channel_id, account_bi)
);

CREATE INDEX IF NOT EXISTS idx_chat_channel_reads_account
  ON chat_channel_reads(account_bi);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:1c97e9dd0950cb536895dd5ef9dfa241b695d23555d31be5226ad29541a996c2',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
