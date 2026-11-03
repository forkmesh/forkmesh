CREATE TABLE IF NOT EXISTS note_chat (
    message_id TEXT PRIMARY KEY,
    note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
    author_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    data TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_note_chat_note
    ON note_chat(note_id, created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:36ec6eeb070a4cabb36f5b533b197e63e82d93fe14494a7910395286e185c4f8',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
