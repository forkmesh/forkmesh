-- Aggregate expected Durable Object platform aborts by minute. A room
-- reconnect storm must not add one actionable Worker-error row per user.
-- The public status sampler consumes only these content-free counters.
CREATE TABLE IF NOT EXISTS durable_object_abort_minute (
    minute_ts INTEGER PRIMARY KEY CHECK (minute_ts >= 0),
    aborts INTEGER NOT NULL DEFAULT 0 CHECK (aborts >= 0),
    duration_aborts INTEGER NOT NULL DEFAULT 0
        CHECK (duration_aborts >= 0),
    updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0)
);
