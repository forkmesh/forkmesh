CREATE TABLE IF NOT EXISTS chat_direct_media (
    media_id TEXT PRIMARY KEY,
    conversation_id TEXT NOT NULL,
    sender_bi TEXT NOT NULL,
    recipient_bi TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN
        ('uploading', 'ready', 'opened', 'expired', 'withdrawn')),
    size INTEGER NOT NULL CHECK (size >= 0),
    parts INTEGER NOT NULL CHECK (parts >= 0),
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    opened_at INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_chat_direct_media_conversation
ON chat_direct_media(conversation_id, created_at);
CREATE INDEX IF NOT EXISTS idx_chat_direct_media_sender
ON chat_direct_media(sender_bi, state);
CREATE INDEX IF NOT EXISTS idx_chat_direct_media_expiry
ON chat_direct_media(state, expires_at);

CREATE TABLE IF NOT EXISTS chat_direct_media_parts (
    media_id TEXT NOT NULL,
    part_index INTEGER NOT NULL CHECK (part_index >= 0),
    size INTEGER NOT NULL CHECK (size >= 0),
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (media_id, part_index)
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:edd807e544f8e1cc4df37b41937f3efbe7188d5cce746d50fb3b0a181dea2098',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
