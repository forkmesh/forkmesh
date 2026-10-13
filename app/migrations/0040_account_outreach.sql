-- ForkMesh D1 migration 0040 — per-user /outreach access flag.
-- The founders /outreach console was previously reachable only by admins and by
-- accounts an admin added to the outreach_team roster. This adds a per-account
-- flag, the operator-settable sibling of is_admin, so a user can be granted the
-- console directly in the database without a roster row.
--
-- Grant outreach access:   UPDATE accounts SET enable_outreach = 1 WHERE name = 'alice';
-- Revoke:                  UPDATE accounts SET enable_outreach = 0 WHERE name = 'alice';
-- (the same UPDATE works on the authoritative `users` row once an account has
-- migrated.)
--
-- The column is added to the existing tables for already-deployed databases;
-- fresh databases get it from ensure_schema() in src/schema.py. SQLite has no
-- "ADD COLUMN IF NOT EXISTS", and D1 runs each migration exactly once, so a plain
-- ADD COLUMN is safe here.

ALTER TABLE accounts ADD COLUMN enable_outreach INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN enable_outreach INTEGER NOT NULL DEFAULT 0;
