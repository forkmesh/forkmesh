-- Per-minute API traffic statistics are no longer stored in D1. The desktop's
-- Network -> Web Requests tab counts them live from the Worker log tail it
-- already streams, so this table cost a batched upsert per few dozen requests
-- plus an hourly prune for numbers that only matter while someone watches.
--
-- The one reader that needed stored counts is the /status sampler's
-- forkmesh/forkmesh visitor evidence, which now reads a failure-only table:
-- a row is written only when a flagship content read answers HTTP 5xx.

DROP TABLE IF EXISTS api_metrics_minute;

CREATE TABLE IF NOT EXISTS flagship_read_failure_minute (
    minute_ts INTEGER PRIMARY KEY CHECK (minute_ts >= 0),
    failures INTEGER NOT NULL DEFAULT 0 CHECK (failures >= 0)
);

-- The runtime schema text changed with it, so stamp the new fingerprint:
INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:8e041a59c64649a71bdbfe1aa8a8e422c08843ad866c8c46b054470f04053ddf',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
