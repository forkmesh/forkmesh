-- ForkMesh D1 migration 0124 — roster-independent /status history.
--
-- The legacy tables store one changed row per system per bucket. That makes
-- the D1 write bill grow linearly with every managed mirror. Batch tables keep
-- one bounded JSON document per UTC minute/hour/day; the Worker dual-reads the
-- legacy rows during their retention window and lets a batch bucket win.

CREATE TABLE IF NOT EXISTS system_status_minute_batch (
    minute_ts INTEGER PRIMARY KEY,
    data      TEXT NOT NULL,
    CHECK(length(data) <= 131072)
);

CREATE TABLE IF NOT EXISTS system_status_hourly_batch (
    hour_ts INTEGER PRIMARY KEY,
    data    TEXT NOT NULL,
    CHECK(length(data) <= 131072)
);

CREATE TABLE IF NOT EXISTS system_status_daily_batch (
    day_ts INTEGER PRIMARY KEY,
    data   TEXT NOT NULL,
    CHECK(length(data) <= 65536)
);

-- Each legacy composite primary key already begins with its UTC bucket.
-- These duplicate time indexes double index maintenance without improving the
-- exact bounded range queries used during the handoff.
DROP INDEX IF EXISTS idx_system_status_minute_ts;
DROP INDEX IF EXISTS idx_system_status_hourly_hour;
DROP INDEX IF EXISTS idx_system_status_daily_day;

-- Bounded status/retention queries that otherwise scan their full tables.
CREATE INDEX IF NOT EXISTS idx_mailtrap_email_sends_sent_at
    ON mailtrap_email_sends(sent_at DESC);
CREATE INDEX IF NOT EXISTS idx_chat_history_ts
    ON chat_history(ts);
CREATE INDEX IF NOT EXISTS idx_nodes_name
    ON nodes(name);

-- This compatibility table historically existed only in the lazy runtime
-- schema. Create it here before indexing so a database built solely from the
-- authoritative native migration chain upgrades successfully too.
CREATE TABLE IF NOT EXISTS repo_state_history (
    key_bi    TEXT NOT NULL,
    state_hash TEXT NOT NULL,
    ts        INTEGER NOT NULL,
    PRIMARY KEY (key_bi, state_hash)
);
CREATE INDEX IF NOT EXISTS idx_repo_state_history_key_ts
    ON repo_state_history(key_bi, ts DESC, state_hash);

-- Native migrations are authoritative for deployed databases.  Append this
-- build's exact DDL fingerprint only after every statement above succeeds so
-- a cold Python Worker takes the one-query schema fast path instead of trying
-- to replay hundreds of compatibility statements past D1's per-invocation
-- query limit.  This must not overwrite the legacy singleton fingerprint:
-- migrations run before traffic switches, so the still-live prior Worker must
-- keep recognizing its own marker throughout the cutover.  Old compatibility
-- markers remain valid for safe rollbacks and later zero-race upgrades.
-- app/tests/test_local_migrations.py ties this value directly to the runtime
-- statement lists and requires a newer migration whenever they change.
INSERT INTO schema_meta (k, v)
VALUES (
    'schema_fingerprint:8814d4b614a16b851fd4dd3075ece77b92ee8ca513694b6789ac04b277a39e70',
    'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
