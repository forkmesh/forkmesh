CREATE TABLE IF NOT EXISTS probe_visitors (visitor_bi TEXT PRIMARY KEY, window_start INTEGER NOT NULL CHECK (window_start >= 0), score INTEGER NOT NULL DEFAULT 0 CHECK (score >= 0), paths TEXT NOT NULL DEFAULT '[]', last_seen INTEGER NOT NULL CHECK (last_seen >= 0), bans INTEGER NOT NULL DEFAULT 0 CHECK (bans >= 0), last_ban_at INTEGER NOT NULL DEFAULT 0 CHECK (last_ban_at >= 0), banned_until INTEGER NOT NULL DEFAULT 0 CHECK (banned_until >= 0));

CREATE INDEX IF NOT EXISTS idx_probe_visitors_last_seen ON probe_visitors(last_seen);

CREATE TABLE IF NOT EXISTS probe_bans (rule_id TEXT PRIMARY KEY, visitor_bi TEXT NOT NULL, client TEXT NOT NULL DEFAULT '', kinds TEXT NOT NULL DEFAULT '', paths TEXT NOT NULL DEFAULT '[]', score INTEGER NOT NULL DEFAULT 0 CHECK (score >= 0), enforced INTEGER NOT NULL DEFAULT 1 CHECK (enforced IN (0, 1)), created_at INTEGER NOT NULL CHECK (created_at >= 0), expires_at INTEGER NOT NULL CHECK (expires_at >= 0), lifted_at INTEGER NOT NULL DEFAULT 0 CHECK (lifted_at >= 0), lifted_by TEXT NOT NULL DEFAULT '');

CREATE INDEX IF NOT EXISTS idx_probe_bans_expires ON probe_bans(expires_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:f52ed5d3bc38da7549ac2d5ca233bcfaeb8ac212442d56b2957d838160c9c778',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
