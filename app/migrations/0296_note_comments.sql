CREATE TABLE IF NOT EXISTS note_comments (
    comment_id TEXT PRIMARY KEY,
    note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
    parent_id TEXT NOT NULL DEFAULT '',
    author_bi TEXT NOT NULL,
    anchor_start INTEGER NOT NULL DEFAULT 0 CHECK (anchor_start >= 0),
    anchor_end INTEGER NOT NULL DEFAULT 0 CHECK (anchor_end >= anchor_start),
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    edited_at INTEGER NOT NULL DEFAULT 0 CHECK (edited_at >= 0),
    resolved_at INTEGER NOT NULL DEFAULT 0 CHECK (resolved_at >= 0),
    resolved_bi TEXT NOT NULL DEFAULT '',
    data TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_note_comments_note
    ON note_comments(note_id, created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:b3d8b67272e68b9f4f61577a856385a287230c33b0b6d8ea6a97ad1971a11d4f',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
