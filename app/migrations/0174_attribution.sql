-- Acquisition attribution (docs/attribution.md).
--
-- One row per anonymous browser session, keyed by the opaque random id in the
-- fm_attr cookie. UTM values are sanitised and allowlisted server-side; the
-- browser never writes them. First-touch is written once and never
-- overwritten; last-touch follows the newest tagged visit. No IP address is
-- stored on a session: the hourly rate rule keeps a daily-salted digest for 24
-- hours in attribution_ip_hourly. user_id is set at signup so reporting joins
-- through the session instead of copying UTMs onto the account record.
CREATE TABLE IF NOT EXISTS attribution_sessions (
    id TEXT PRIMARY KEY,
    first_seen_at INTEGER NOT NULL CHECK (first_seen_at > 0),
    last_seen_at INTEGER NOT NULL CHECK (last_seen_at > 0),
    first_utm_source TEXT, first_utm_medium TEXT, first_utm_campaign TEXT,
    first_utm_term TEXT, first_utm_content TEXT,
    last_utm_source TEXT, last_utm_medium TEXT, last_utm_campaign TEXT,
    last_utm_term TEXT, last_utm_content TEXT,
    first_landing_path TEXT NOT NULL DEFAULT '',
    first_referrer_host TEXT NOT NULL DEFAULT '',
    is_bot INTEGER NOT NULL DEFAULT 0 CHECK (is_bot IN (0, 1)),
    bot_reason TEXT NOT NULL DEFAULT '',
    user_id TEXT REFERENCES users(user_bi) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_attribution_sessions_first_seen
    ON attribution_sessions(first_seen_at, id);
CREATE INDEX IF NOT EXISTS idx_attribution_sessions_user
    ON attribution_sessions(user_id);

CREATE TABLE IF NOT EXISTS attribution_events (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL REFERENCES attribution_sessions(id)
        ON DELETE CASCADE,
    event_name TEXT NOT NULL,
    occurred_at INTEGER NOT NULL CHECK (occurred_at > 0),
    metadata TEXT NOT NULL DEFAULT '{}'
);

CREATE INDEX IF NOT EXISTS idx_attribution_events_name_time
    ON attribution_events(event_name, occurred_at);
CREATE INDEX IF NOT EXISTS idx_attribution_events_session
    ON attribution_events(session_id);
CREATE INDEX IF NOT EXISTS idx_attribution_events_time
    ON attribution_events(occurred_at, id);

-- Sessions per source address per hour. The digest's HMAC input includes the
-- UTC day (a rotating daily salt); rows older than 24h are purged hourly.
CREATE TABLE IF NOT EXISTS attribution_ip_hourly (
    ip_hash TEXT NOT NULL,
    hour_bucket INTEGER NOT NULL,
    sessions INTEGER NOT NULL DEFAULT 0,
    session_ids TEXT NOT NULL DEFAULT '[]',
    PRIMARY KEY (ip_hash, hour_bucket)
);

-- Requests refused before a session existed (known crawlers), per UTC day and
-- reason, so the transparency page can state what was filtered.
CREATE TABLE IF NOT EXISTS attribution_filtered_hits (
    day TEXT NOT NULL,
    reason TEXT NOT NULL,
    hits INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (day, reason)
);

-- Daily snapshot of cumulative mirror_serve_counters clone totals per
-- repository. Per-day clones are the delta between consecutive snapshots; the
-- clone-serving path itself is never touched.
CREATE TABLE IF NOT EXISTS attribution_clone_snapshots (
    day TEXT NOT NULL,
    owner TEXT NOT NULL,
    repo TEXT NOT NULL,
    clones_total INTEGER NOT NULL DEFAULT 0,
    captured_at INTEGER NOT NULL,
    PRIMARY KEY (day, owner, repo)
);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:a7cb9b5ed6f7f89231c869d0cf97d2d668823d949fcee3807d9fcedc9696048e',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
