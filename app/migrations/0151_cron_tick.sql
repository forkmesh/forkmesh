-- One row per maintenance cron tick: how many batch Durable Objects it took,
-- how many phases ran, what they cost in subrequests, and a bounded JSON
-- detail of the per-phase measurements and failures.
--
-- Durable Object storage cannot answer "what has the cron been doing": it is
-- per-object and evicted between invocations. This table is the history the
-- admin cron report reads, and the measurements the scheduler's packing is
-- judged by. The primary key is the minute slot, so a replayed tick overwrites
-- instead of accumulating, and rows age out on a six-hour window swept twice
-- an hour by the dispatcher itself.
CREATE TABLE IF NOT EXISTS cron_tick (
    slot INTEGER PRIMARY KEY,
    ts INTEGER NOT NULL,
    batches INTEGER NOT NULL DEFAULT 0,
    phases INTEGER NOT NULL DEFAULT 0,
    failures INTEGER NOT NULL DEFAULT 0,
    subrequests INTEGER NOT NULL DEFAULT 0,
    ms INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    CHECK (length(data) <= 8192));

CREATE INDEX IF NOT EXISTS idx_cron_tick_ts ON cron_tick(ts);

-- Stamp the schema fingerprint this migration brings the database to, so cold
-- isolates take the schema_meta fast path instead of replaying every DDL
-- statement on their first request.
INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:d42d63cc68c32e9b04df0f0143c878081b4337fad7302ab13088786fdab6f21f',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
