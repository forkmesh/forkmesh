CREATE TABLE IF NOT EXISTS chat_channel_invites (
  channel_id    TEXT PRIMARY KEY,
  token_bi      TEXT NOT NULL UNIQUE,
  data          TEXT NOT NULL,
  created_by_bi TEXT NOT NULL,
  created_at    INTEGER NOT NULL
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:ec9661a0b8b21bff9b7d70d74ef13dd304a8b40194bbe9b65314cc4720dc7c2e',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
