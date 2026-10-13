-- At-most-once ownership of each minute's /status health sample. The platform
-- Cron Trigger now records the sample directly (in addition to the
-- ForkMeshCronRunner alarm batch), so the minute strip stays green even while
-- Durable Objects are wedged or over their free-tier quota. Whichever caller
-- INSERTs a minute's claim row first owns that sample; the loser skips it so
-- the hourly/daily checks counters never double-count a minute.
CREATE TABLE IF NOT EXISTS system_status_sample_claim (
    minute_ts INTEGER PRIMARY KEY, claim TEXT NOT NULL,
    claimed_at INTEGER NOT NULL);
