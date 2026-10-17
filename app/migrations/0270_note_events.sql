CREATE TABLE IF NOT EXISTS note_events (
    id INTEGER PRIMARY KEY,
    note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
    kind TEXT NOT NULL,
    actor_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    last_at INTEGER NOT NULL CHECK (last_at >= 0),
    hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1),
    detail TEXT NOT NULL DEFAULT '{}'
);

CREATE INDEX IF NOT EXISTS idx_note_events_note
    ON note_events(note_id, last_at DESC);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:1376a9e04476130360fe7ebf9fcb641b8288b2e5010ee038a255437fff2df918',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
