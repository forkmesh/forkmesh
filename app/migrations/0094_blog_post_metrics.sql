-- Aggregate blog reach. A post keeps one total counter, a fixed 64-register
-- HyperLogLog sketch, and aggregate referring-host counters. No IP address,
-- user agent, cookie, account, session, path beyond the public post slug, or
-- per-visitor digest is retained.
CREATE TABLE IF NOT EXISTS blog_post_metrics (
    slug TEXT PRIMARY KEY,
    views INTEGER NOT NULL DEFAULT 0 CHECK (views >= 0),
    updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0)
);

CREATE TABLE IF NOT EXISTS blog_post_unique_hll (
    slug TEXT NOT NULL,
    register_id INTEGER NOT NULL CHECK (register_id >= 0 AND register_id < 64),
    rank INTEGER NOT NULL CHECK (rank >= 1 AND rank <= 251),
    PRIMARY KEY (slug, register_id)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS blog_post_referrers (
    slug TEXT NOT NULL,
    host TEXT NOT NULL,
    visits INTEGER NOT NULL DEFAULT 0 CHECK (visits >= 0),
    last_ts INTEGER NOT NULL DEFAULT 0 CHECK (last_ts >= 0),
    PRIMARY KEY (slug, host)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_blog_post_referrers_rank
ON blog_post_referrers(slug, visits DESC, last_ts DESC);
