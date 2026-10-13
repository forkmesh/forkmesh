CREATE TABLE IF NOT EXISTS not_found_paths (
  site TEXT NOT NULL,
  method TEXT NOT NULL,
  path TEXT NOT NULL,
  hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1),
  first_seen INTEGER NOT NULL CHECK (first_seen >= 0),
  last_seen INTEGER NOT NULL CHECK (last_seen >= 0),
  referrer TEXT NOT NULL DEFAULT '',
  alerted_at INTEGER NOT NULL DEFAULT 0 CHECK (alerted_at >= 0),
  PRIMARY KEY (site, method, path)
);

CREATE INDEX IF NOT EXISTS idx_not_found_paths_last_seen
  ON not_found_paths(last_seen);

CREATE INDEX IF NOT EXISTS idx_not_found_paths_hits
  ON not_found_paths(hits, last_seen);

CREATE INDEX IF NOT EXISTS idx_not_found_paths_alerted
  ON not_found_paths(alerted_at) WHERE alerted_at>0;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:1075c3d7eb563531fd057a494f33b83f52b0c3e91be4ef68bc0a66b51b9dd75f',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
