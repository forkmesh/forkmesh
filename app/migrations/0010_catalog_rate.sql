-- ForkMesh D1 migration 0010 — catalog write throttle.
-- Records the last catalog-write time per owner (blind index) so a single key
-- can't flood the catalog. The worker also creates this lazily (ensure_schema in
-- src/entry.py); this migration keeps the file-based schema in sync. Plaintext
-- timestamp only — no user content.

CREATE TABLE IF NOT EXISTS catalog_rate (
    owner_bi TEXT PRIMARY KEY,   -- blind index of the owner account name
    ts INTEGER NOT NULL          -- epoch ms of the last catalog write
);
