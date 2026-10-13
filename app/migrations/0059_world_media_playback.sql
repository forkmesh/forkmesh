-- Server-authoritative shared playback coordination.
--
-- This table stores only an external playlist item reference and a clock
-- position. It never stores, proxies, autoplays, records, or rebroadcasts
-- media. Each browser remains responsible for explicit local playback consent.
CREATE TABLE IF NOT EXISTS world_media_playback (
    space_id TEXT PRIMARY KEY,
    item_id TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT 'idle'
        CHECK (state IN ('idle', 'playing', 'paused', 'stopped')),
    position_ms INTEGER NOT NULL DEFAULT 0
        CHECK (position_ms >= 0 AND position_ms <= 604800000),
    started_at INTEGER NOT NULL DEFAULT 0,
    changed_at INTEGER NOT NULL,
    changed_by_bi TEXT NOT NULL DEFAULT '',
    revision INTEGER NOT NULL DEFAULT 0 CHECK (revision >= 0),
    change_id TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_world_media_playback_changed
    ON world_media_playback(changed_at);
