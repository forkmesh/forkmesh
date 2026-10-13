-- Durable, provider-neutral repository migration state (issue #477).
--
-- Job and object payloads are encrypted by the Worker.  Provider object ids,
-- private repository names, checkpoints, errors, and reports therefore never
-- appear in plaintext D1 columns.  source_key is a one-way SHA-256 identity
-- used for idempotent upserts and later two-way synchronization.

CREATE TABLE IF NOT EXISTS repository_migration_jobs (
    id TEXT PRIMARY KEY,
    repo_id TEXT NOT NULL,
    owner_bi TEXT NOT NULL,
    provider TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    phase TEXT NOT NULL DEFAULT 'git_refs',
    generation INTEGER NOT NULL DEFAULT 1,
    setup_ms INTEGER NOT NULL DEFAULT 0,
    transfer_started_at INTEGER NOT NULL DEFAULT 0,
    completed_at INTEGER NOT NULL DEFAULT 0,
    retry_count INTEGER NOT NULL DEFAULT 0,
    object_count INTEGER NOT NULL DEFAULT 0,
    state_hash TEXT NOT NULL DEFAULT '',
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (repo_id)
);
CREATE INDEX IF NOT EXISTS idx_repository_migration_jobs_owner
    ON repository_migration_jobs(owner_bi, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_repository_migration_jobs_status
    ON repository_migration_jobs(status, updated_at);

CREATE TABLE IF NOT EXISTS repository_migration_objects (
    job_id TEXT NOT NULL,
    object_type TEXT NOT NULL,
    source_key TEXT NOT NULL,
    payload_hash TEXT NOT NULL,
    generation INTEGER NOT NULL,
    data TEXT NOT NULL,
    imported_at INTEGER NOT NULL,
    PRIMARY KEY (job_id, object_type, source_key)
);
CREATE INDEX IF NOT EXISTS idx_repository_migration_objects_generation
    ON repository_migration_objects(job_id, generation, object_type);

-- One row per transfer generation.  It contains only coarse plaintext
-- operational counters; the detailed fidelity/recovery report is encrypted.
CREATE TABLE IF NOT EXISTS repository_migration_runs (
    job_id TEXT NOT NULL,
    generation INTEGER NOT NULL,
    provider TEXT NOT NULL,
    outcome TEXT NOT NULL DEFAULT 'running',
    setup_ms INTEGER NOT NULL DEFAULT 0,
    transfer_ms INTEGER NOT NULL DEFAULT 0,
    retry_count INTEGER NOT NULL DEFAULT 0,
    object_count INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    started_at INTEGER NOT NULL,
    finished_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (job_id, generation)
);
CREATE INDEX IF NOT EXISTS idx_repository_migration_runs_metrics
    ON repository_migration_runs(finished_at DESC, provider, outcome);

-- Deleting an external listing is an explicit owner action. Keep that action
-- complete without relying on a best-effort series of Worker requests.
CREATE TRIGGER IF NOT EXISTS trg_repository_import_delete_migration
AFTER DELETE ON repository_imports
BEGIN
    DELETE FROM repository_migration_objects
    WHERE job_id IN (
        SELECT id FROM repository_migration_jobs WHERE repo_id=OLD.id
    );
    DELETE FROM repository_migration_runs
    WHERE job_id IN (
        SELECT id FROM repository_migration_jobs WHERE repo_id=OLD.id
    );
    DELETE FROM repository_migration_jobs WHERE repo_id=OLD.id;
END;

-- Stamp the exact lazy runtime DDL shipped with this migration so a cold
-- isolate can take ensure_schema's verified fast path.
INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:8b40e63418b2a99c07146447d93bc0598ca19c64679d94b0a227088c9ff8d0b1',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
