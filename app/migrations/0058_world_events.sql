-- Public UTC community-event announcements. Only platform administrators may
-- mutate them; public reads expose bounded announcement metadata and omit the
-- author blind index. Cancellation is retained briefly for the private audit
-- trail and never rendered as a live event.
CREATE TABLE IF NOT EXISTS world_events (
    event_id TEXT PRIMARY KEY,
    status TEXT NOT NULL DEFAULT 'scheduled'
        CHECK (status IN ('scheduled', 'cancelled')),
    starts_at INTEGER NOT NULL,
    ends_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    created_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    cancelled_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_world_events_public
    ON world_events(status, ends_at, starts_at);
CREATE INDEX IF NOT EXISTS idx_world_events_retention
    ON world_events(status, cancelled_at, ends_at);
CREATE TRIGGER IF NOT EXISTS trg_world_events_record_limit
    BEFORE INSERT ON world_events
    WHEN (SELECT COUNT(*) FROM world_events) >= 500
    BEGIN
        SELECT RAISE(ABORT, 'world_event_catalog_full');
    END;
