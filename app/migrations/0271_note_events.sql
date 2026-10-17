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
  'schema_fingerprint:10fa5453dc8a96ce2e6ddaaf856cad1eb790120ee6e9218b92c067248b100bf0',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
