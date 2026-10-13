-- ForkMesh D1 migration 0007 — hourly online-activity samples.
-- A per-minute cron (the Worker's scheduled handler) adds the current count of
-- online nodes into the current hour's bucket. node_minutes is therefore
-- "node-minutes online" that hour (one node online for the whole hour = 60).
-- Drives the 24-hour online-activity graph on /network/. The worker also
-- creates this via ensure_schema in src/entry.py.

CREATE TABLE IF NOT EXISTS online_hourly (
  hour_ts      INTEGER PRIMARY KEY,        -- epoch ms at the start of the hour
  node_minutes INTEGER NOT NULL DEFAULT 0  -- summed online-node count per minute
);
