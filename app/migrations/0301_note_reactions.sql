CREATE TABLE IF NOT EXISTS note_reactions (
    reaction_id TEXT PRIMARY KEY,
    note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
    author_bi TEXT NOT NULL,
    anchor_start INTEGER NOT NULL DEFAULT 0 CHECK (anchor_start >= 0),
    anchor_end INTEGER NOT NULL DEFAULT 0 CHECK (anchor_end >= anchor_start),
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    data TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_note_reactions_note
    ON note_reactions(note_id, created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:09aa3734fb747fb043555339aff41b8becb97ab68678268b5b7e611bd9ef3e31',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
