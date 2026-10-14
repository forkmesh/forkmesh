ALTER TABLE chat_direct_participants
  ADD COLUMN unread_reminded_at INTEGER NOT NULL DEFAULT 0
  CHECK (unread_reminded_at >= 0);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:f6b1f7f7a4bbf3af12a47342f9531f10f9c37d42ef0558873cfce3422eb4fd8f',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
