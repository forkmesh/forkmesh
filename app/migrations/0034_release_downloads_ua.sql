-- ForkMesh D1 migration 0034 — capture the User-Agent on release downloads.
-- The admin dashboard lists release_downloads as a plain event log (same
-- treatment as install_diag); operators asked to see the requester's
-- User-Agent in that list to spot scripted/bot download traffic.
--
-- The column is added to the existing table for already-deployed databases;
-- fresh databases get it from ensure_schema() in src/entry.py. SQLite has no
-- "ADD COLUMN IF NOT EXISTS", and D1 runs each migration exactly once, so a
-- plain ADD COLUMN is safe here.

-- Local Wrangler databases run migrations before the Worker has a chance to
-- call ensure_schema(). Bootstrap the table there while preserving the
-- existing production path, where the table already exists.
CREATE TABLE IF NOT EXISTS release_downloads (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_bi TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    ts INTEGER NOT NULL
);

ALTER TABLE release_downloads ADD COLUMN ua TEXT;
