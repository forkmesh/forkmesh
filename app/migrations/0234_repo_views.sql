CREATE TABLE IF NOT EXISTS repo_views (repo_bi TEXT NOT NULL, viewer_key TEXT NOT NULL, first_at INTEGER NOT NULL CHECK (first_at >= 0), last_at INTEGER NOT NULL CHECK (last_at >= 0), counted_at INTEGER NOT NULL DEFAULT 0 CHECK (counted_at >= 0), hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1), PRIMARY KEY (repo_bi, viewer_key)) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS repo_view_totals (repo_bi TEXT PRIMARY KEY, views INTEGER NOT NULL DEFAULT 0 CHECK (views >= 0), viewers INTEGER NOT NULL DEFAULT 0 CHECK (viewers >= 0), last_viewed_at INTEGER NOT NULL DEFAULT 0 CHECK (last_viewed_at >= 0));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:e8042483075282829fcb369088f81e87a2b04eefcb83beeb84adc9f36e4145fb',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
