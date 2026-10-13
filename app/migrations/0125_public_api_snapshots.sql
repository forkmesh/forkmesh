-- ForkMesh D1 migration 0125 — consolidate public API snapshots into main D1.
--
-- The App Worker now owns both generation and serving of these five bounded,
-- public-only JSON projections.  A newly migrated main database starts empty;
-- the scheduled publisher repopulates all rows for the active deployment.

CREATE TABLE IF NOT EXISTS public_api_snapshots (
    path TEXT PRIMARY KEY NOT NULL,
    schema_version INTEGER NOT NULL,
    kind TEXT NOT NULL,
    generated_at INTEGER NOT NULL,
    fresh_until INTEGER NOT NULL,
    stale_until INTEGER NOT NULL,
    deploy_fingerprint TEXT NOT NULL DEFAULT '',
    content_length INTEGER NOT NULL,
    payload TEXT NOT NULL,
    CHECK (content_length > 1 AND content_length <= 1572864),
    -- The publisher uses ensure_ascii=True, so SQLite character length and
    -- UTF-8 byte length are identical for every accepted payload.
    CHECK (length(payload) = content_length),
    CHECK (fresh_until >= generated_at),
    CHECK (stale_until = fresh_until)
) WITHOUT ROWID;

-- This compatibility table historically existed only in the lazy runtime
-- schema. Create it before indexing so the authoritative native migration
-- chain is also sufficient for a fresh database. Mirrors payloads select one
-- repository and a bounded owner-alias set; the historical primary key begins
-- with node_name and therefore cannot narrow that lookup.
CREATE TABLE IF NOT EXISTS mirror_serve_counters (
    node_name TEXT NOT NULL,
    owner TEXT NOT NULL,
    repo TEXT NOT NULL,
    clones INTEGER NOT NULL DEFAULT 0,
    website INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (node_name, owner, repo)
);
CREATE INDEX IF NOT EXISTS idx_mirror_serve_counters_repo_owner_node
    ON mirror_serve_counters(repo, owner, node_name);

-- Native migrations are authoritative for deployed databases. Append this
-- build's exact runtime DDL fingerprint only after the table exists. Preserve
-- the legacy singleton fingerprint so a still-live prior Worker remains safe
-- throughout a rolling cutover; compatibility markers are append-only.
INSERT INTO schema_meta (k, v)
VALUES (
    'schema_fingerprint:c2dae160cb7342ec6eee55e9c32cd7187247b1130026cc542102b7e51256f984',
    'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
