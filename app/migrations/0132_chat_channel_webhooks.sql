-- User-managed chat channels and token-addressed incoming webhooks.
-- The webhook bearer token is retained only as a keyed blind index. Its name
-- and creator stay in the encrypted data envelope.

CREATE TABLE IF NOT EXISTS chat_channel_webhooks (
  webhook_id    TEXT PRIMARY KEY,
  channel_id    TEXT NOT NULL,
  token_bi      TEXT NOT NULL UNIQUE,
  data          TEXT NOT NULL,
  created_by_bi TEXT NOT NULL,
  created_at    INTEGER NOT NULL,
  last_used_at  INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_chat_channel_webhooks_channel
  ON chat_channel_webhooks(channel_id, created_at);

-- Stamp the exact lazy-runtime DDL only after this migration's tables exist.
INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:eed8284a9b146d0f688bf05d1ecc9d05ffa462355ef0170dc1943d794422e2d9',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
