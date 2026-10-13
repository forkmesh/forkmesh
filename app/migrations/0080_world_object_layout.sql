-- Shared, administrator-curated placement overrides for the fixed Town
-- Square scene objects. One row per scene object id holding only ground
-- coordinates; no visitor, account, or session data is stored here.
CREATE TABLE IF NOT EXISTS world_object_layout (
    object_id TEXT PRIMARY KEY,
    x REAL NOT NULL,
    z REAL NOT NULL,
    updated_by_bi TEXT NOT NULL,
    updated_at INTEGER NOT NULL);
