-- Web edits of the repo About card (about text + website) queued for the
-- owner's desktop node, which drains them via GET /api/sync and writes the
-- change into the repo's committed .forkmesh/info.json. Latest edit wins.
CREATE TABLE IF NOT EXISTS about_inbox (
    repo_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
    queued_at INTEGER NOT NULL);
