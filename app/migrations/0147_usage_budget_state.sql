-- Free-plan usage sampler state for the usage-budget cron phase: one row
-- per monitored Cloudflare daily allowance (Durable Object duration and
-- requests, Worker requests). Holds the current UTC day, the last sampled
-- percentage, and the highest threshold already Pinged today so each of the
-- 10/50/80/90% levels alerts every platform admin exactly once per day.
CREATE TABLE IF NOT EXISTS usage_budget_state (
    metric TEXT PRIMARY KEY,
    day TEXT NOT NULL,
    percent REAL NOT NULL DEFAULT 0,
    pinged_threshold INTEGER NOT NULL DEFAULT 0,
    sampled_at INTEGER NOT NULL DEFAULT 0
);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:72e5d755e0b13aec5000de79d27627446801d7f9fe01d7f142d6c950548cfdd0',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
