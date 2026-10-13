CREATE TABLE IF NOT EXISTS instance_directory (
    domain TEXT PRIMARY KEY,
    id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    url TEXT NOT NULL,
    logo TEXT NOT NULL DEFAULT '',
    world INTEGER NOT NULL DEFAULT 0 CHECK (world IN (0, 1)),
    listed INTEGER NOT NULL DEFAULT 0 CHECK (listed IN (0, 1)),
    slot INTEGER NOT NULL DEFAULT -1,
    hidden INTEGER NOT NULL DEFAULT 0 CHECK (hidden IN (0, 1)),
    failures INTEGER NOT NULL DEFAULT 0,
    offline_since INTEGER NOT NULL DEFAULT 0,
    checked_at INTEGER NOT NULL DEFAULT 0,
    pinged_at INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_instance_directory_created
    ON instance_directory(created_at);
CREATE INDEX IF NOT EXISTS idx_instance_directory_checked
    ON instance_directory(checked_at);
