CREATE TABLE IF NOT EXISTS issue_views (repo_bi TEXT NOT NULL, number INTEGER NOT NULL CHECK (number > 0), viewer_key TEXT NOT NULL, first_at INTEGER NOT NULL CHECK (first_at >= 0), last_at INTEGER NOT NULL CHECK (last_at >= 0), counted_at INTEGER NOT NULL DEFAULT 0 CHECK (counted_at >= 0), hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1), PRIMARY KEY (repo_bi, number, viewer_key)) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS issue_view_totals (repo_bi TEXT NOT NULL, number INTEGER NOT NULL CHECK (number > 0), views INTEGER NOT NULL DEFAULT 0 CHECK (views >= 0), viewers INTEGER NOT NULL DEFAULT 0 CHECK (viewers >= 0), last_viewed_at INTEGER NOT NULL DEFAULT 0 CHECK (last_viewed_at >= 0), PRIMARY KEY (repo_bi, number)) WITHOUT ROWID;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:c841261a4d250195503c8582103050794b077a64668e7d1060abe2c94466bceb',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
