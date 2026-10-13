-- ForkMesh D1 migration 0022 — public /status page (30-day history per system).
-- Backs the per-minute cron that folds one health check per system into
-- today's UTC-day bucket (record_status_sample in src/entry.py). The worker
-- also creates this lazily (ensure_schema); this migration keeps the
-- file-based schema in sync.
--
-- checks/failures let the page compute an uptime percentage per day without
-- storing every individual sample. day_ts is the epoch-ms start of the UTC
-- day; system is one of the ids in STATUS_SYSTEMS (website/api/database/
-- git_hosting/realtime).

CREATE TABLE IF NOT EXISTS system_status_daily (
    day_ts   INTEGER NOT NULL,
    system   TEXT NOT NULL,
    checks   INTEGER NOT NULL DEFAULT 0,
    failures INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (day_ts, system)
);

CREATE INDEX IF NOT EXISTS idx_system_status_daily_day ON system_status_daily(day_ts);
