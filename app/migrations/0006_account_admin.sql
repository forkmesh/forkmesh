-- ForkMesh D1 migration 0006 — operator-settable admin flag.
-- Admins were previously only the node names in the ADMIN_NODES var. This adds a
-- per-account flag so an admin can also be granted directly in the database, and
-- a plaintext `name` column (the node name is public) so the row is findable by
-- name. The worker also creates these via ensure_schema in src/entry.py.
--
-- Grant admin to a user:   UPDATE accounts SET is_admin = 1 WHERE name = 'alice';
-- Revoke:                  UPDATE accounts SET is_admin = 0 WHERE name = 'alice';
-- (name is populated the next time the account is written; active nodes refresh
-- it on their heartbeat.)

ALTER TABLE accounts ADD COLUMN name TEXT;
ALTER TABLE accounts ADD COLUMN is_admin INTEGER NOT NULL DEFAULT 0;
