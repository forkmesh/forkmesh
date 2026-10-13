CREATE TABLE IF NOT EXISTS world_floor_notes (
    note_id TEXT PRIMARY KEY CHECK (length(note_id) = 32),
    account_bi TEXT NOT NULL,
    x REAL NOT NULL CHECK (x BETWEEN -1500 AND 1500),
    y REAL NOT NULL CHECK (y BETWEEN -20 AND 200),
    z REAL NOT NULL CHECK (z BETWEEN -1500 AND 1500),
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    reply_count INTEGER NOT NULL DEFAULT 0 CHECK (reply_count >= 0),
    data TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_world_floor_notes_updated
    ON world_floor_notes(updated_at DESC, note_id DESC);

CREATE INDEX IF NOT EXISTS idx_world_floor_notes_account
    ON world_floor_notes(account_bi, created_at DESC);

CREATE TABLE IF NOT EXISTS world_floor_note_replies (
    reply_id TEXT PRIMARY KEY CHECK (length(reply_id) = 32),
    note_id TEXT NOT NULL CHECK (length(note_id) = 32),
    account_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    data TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_world_floor_note_replies_note
    ON world_floor_note_replies(note_id, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_world_floor_note_replies_account
    ON world_floor_note_replies(account_bi, created_at DESC);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:e0c34f91c012eb7f652311c5ed1d93d5d7c09247d84340a945d94a1bff5d65b3',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
