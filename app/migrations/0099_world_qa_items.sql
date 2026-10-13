-- Completed build-board work promoted into the shared QA deck. Content is
-- bounded manager-authored test guidance; votes remain account-scoped in
-- world_qa_reviews and are exposed only as global aggregate counts.
CREATE TABLE IF NOT EXISTS world_qa_items (
    item_key TEXT PRIMARY KEY
        CHECK (length(item_key) BETWEEN 1 AND 80),
    title TEXT NOT NULL CHECK (length(title) BETWEEN 1 AND 160),
    how_to_test TEXT NOT NULL
        CHECK (length(how_to_test) BETWEEN 1 AND 720),
    source_key TEXT NOT NULL DEFAULT ''
        CHECK (length(source_key) <= 80),
    added_at INTEGER NOT NULL CHECK (added_at >= 0),
    active INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0,1))
);

CREATE INDEX IF NOT EXISTS idx_world_qa_items_active_time
    ON world_qa_items(active, added_at DESC);
