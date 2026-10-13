-- ForkMesh D1 migration 0023 — hourly breakdown backing the per-day sliver
-- bars on /status. Folded by the same per-minute cron as system_status_daily
-- (record_status_sample in src/entry.py); the worker also creates this lazily
-- (ensure_schema), this migration keeps the file-based schema in sync.
--
-- hour_ts is the epoch-ms start of the UTC hour. reason is a short
-- human-readable note on the most recent failing check that hour (e.g. which
-- path/host check failed), so hovering a degraded/down hour on the status
-- page explains why instead of just showing a color.

CREATE TABLE IF NOT EXISTS system_status_hourly (
    hour_ts  INTEGER NOT NULL,
    system   TEXT NOT NULL,
    checks   INTEGER NOT NULL DEFAULT 0,
    failures INTEGER NOT NULL DEFAULT 0,
    reason   TEXT,
    PRIMARY KEY (hour_ts, system)
);

CREATE INDEX IF NOT EXISTS idx_system_status_hourly_hour ON system_status_hourly(hour_ts);
