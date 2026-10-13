-- ForkMesh D1 migration 0011 — repository visibility (private repos).
-- Adds a plaintext fast-lookup flag so the relay can decide whether a repo is
-- private without decrypting its row: private repos are omitted from the public
-- catalog GET and require an owner-key-signed forkmesh-view-v1 token on
-- browse/clone. The worker also adds this lazily (ensure_schema in src/entry.py);
-- this migration keeps the file-based schema in sync. No user content — just a
-- boolean flag mirrored from the encrypted record's "visibility" field.

ALTER TABLE repositories ADD COLUMN is_private INTEGER NOT NULL DEFAULT 0;
