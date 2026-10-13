CREATE TABLE IF NOT EXISTS not_found_hours (
  site TEXT NOT NULL,
  method TEXT NOT NULL,
  path TEXT NOT NULL,
  hour INTEGER NOT NULL CHECK (hour >= 0),
  hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1),
  PRIMARY KEY (site, method, path, hour)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_not_found_hours_hour
  ON not_found_hours(hour);

CREATE TABLE IF NOT EXISTS not_found_ignores (
  site TEXT NOT NULL,
  path TEXT NOT NULL,
  prefix INTEGER NOT NULL DEFAULT 0 CHECK (prefix IN (0, 1)),
  created_at INTEGER NOT NULL CHECK (created_at >= 0),
  created_by TEXT NOT NULL DEFAULT '',
  hits INTEGER NOT NULL DEFAULT 0 CHECK (hits >= 0),
  last_seen INTEGER NOT NULL DEFAULT 0 CHECK (last_seen >= 0),
  PRIMARY KEY (site, path)
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:4714e19ba6561716d753ff088486ac70be882ba48f1616fb2682bd69aed075d2',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
