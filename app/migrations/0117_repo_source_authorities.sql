-- Keep one durable source-of-truth device per logical owner/repository while
-- retaining later same-account devices as serving mirrors.
CREATE TABLE IF NOT EXISTS repo_source_authorities (
    repo_bi TEXT PRIMARY KEY,
    node_id TEXT NOT NULL,
    machine_name TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS repo_device_mirrors (
    repo_bi TEXT NOT NULL,
    node_id TEXT NOT NULL,
    data TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, node_id)
);

CREATE INDEX IF NOT EXISTS idx_repo_device_mirrors_repo
    ON repo_device_mirrors(repo_bi, updated_at DESC);
