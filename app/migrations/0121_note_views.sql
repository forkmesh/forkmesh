-- Read counts for published notes. One row per note per blinded reader, so
-- the owner sees both distinct readers and total reads without the database
-- ever retaining an address: `viewer_key` is a per-note one-way digest.
CREATE TABLE IF NOT EXISTS note_views (
    note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
    viewer_key TEXT NOT NULL,
    first_at INTEGER NOT NULL CHECK (first_at >= 0),
    last_at INTEGER NOT NULL CHECK (last_at >= 0),
    hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1),
    PRIMARY KEY(note_id, viewer_key)
);
CREATE INDEX IF NOT EXISTS idx_note_views_note
    ON note_views(note_id, last_at DESC);
