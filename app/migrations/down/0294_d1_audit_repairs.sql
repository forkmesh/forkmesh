-- Reverse 0294_d1_audit_repairs.sql. The catch-up tables stay: schema.py
-- creates them at runtime too, and they hold live rows. commit_inbox comes back
-- empty, as 0009 made it; forkmesh_d1_migrations is not recreated (nothing
-- reads it). Restore their rows from the nightly backup if they are needed.

CREATE TABLE IF NOT EXISTS commit_inbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_bi TEXT NOT NULL,
    data TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_commit_inbox_repo ON commit_inbox(repo_bi);

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:f98c955c26168b8c22df7e6675d0984c66b0fce8a506edcbd698e8c0f48ccc1a';
