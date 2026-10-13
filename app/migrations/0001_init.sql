-- ForkMesh D1 schema — encrypted at rest.
-- Applied automatically by ../migrate.sh (hooked into wrangler.toml [build]).
-- The worker also creates these lazily (ensure_schema in src/entry.py).
--
-- Every table stores only HMAC "blind index" columns (for lookups/uniqueness)
-- plus a single AES-GCM-encrypted JSON `data` blob — no plaintext content.
-- The worker holds DATA_KEY: this is encryption at rest, not zero-knowledge.
-- Add later changes as 0002_*.sql, etc. Run once each via `d1 migrations apply`.

-- One-time: drop any superseded plaintext tables from before the encrypted
-- schema (safe — this migration is recorded and never re-run; no-op on new DBs).
DROP TABLE IF EXISTS accounts;
DROP TABLE IF EXISTS repositories;
DROP TABLE IF EXISTS issue_inbox;
DROP TABLE IF EXISTS pull_inbox;

-- Accounts = node = user/organization. data = {name, pubkey, email, solana,
-- pass_salt, pass_hash, totp_secret, totp_enrolled, created_at}.
CREATE TABLE IF NOT EXISTS accounts (
  name_bi TEXT PRIMARY KEY,   -- blind_index(name)
  data    TEXT NOT NULL       -- AES-GCM encrypted JSON
);

-- Repository catalog, namespaced under the owning account; one row per owner/name.
CREATE TABLE IF NOT EXISTS repositories (
  key_bi   TEXT PRIMARY KEY,  -- blind_index("owner/name")
  owner_bi TEXT NOT NULL,     -- blind_index(owner)
  data     TEXT NOT NULL      -- AES-GCM encrypted catalog record (incl. updatedAt)
);
CREATE INDEX IF NOT EXISTS idx_repos_owner ON repositories(owner_bi);

-- Issue submission inbox (one row per pending signed event).
CREATE TABLE IF NOT EXISTS issue_inbox (
  id      INTEGER PRIMARY KEY AUTOINCREMENT,
  repo_bi TEXT NOT NULL,      -- blind_index("owner/repo")
  data    TEXT NOT NULL       -- AES-GCM encrypted submission
);
CREATE INDEX IF NOT EXISTS idx_issue_inbox_repo ON issue_inbox(repo_bi);

-- Pull-request submission inbox.
CREATE TABLE IF NOT EXISTS pull_inbox (
  id      INTEGER PRIMARY KEY AUTOINCREMENT,
  repo_bi TEXT NOT NULL,      -- blind_index("owner/repo")
  data    TEXT NOT NULL       -- AES-GCM encrypted submission
);
CREATE INDEX IF NOT EXISTS idx_pull_inbox_repo ON pull_inbox(repo_bi);
