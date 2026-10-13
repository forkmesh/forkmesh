-- Public, content-free deployment lifecycle for the World refresh banner.
-- The deploy script writes this row directly through authenticated Wrangler;
-- browsers can only read it through the public GET endpoint.
CREATE TABLE IF NOT EXISTS world_deploy_status (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    state TEXT NOT NULL CHECK (state IN ('idle', 'deploying', 'ready', 'failed')),
    revision TEXT NOT NULL DEFAULT '' CHECK (length(revision) <= 96),
    started_at INTEGER NOT NULL DEFAULT 0 CHECK (started_at >= 0),
    finished_at INTEGER NOT NULL DEFAULT 0 CHECK (finished_at >= 0)
);

INSERT OR IGNORE INTO world_deploy_status
    (singleton, state, revision, started_at, finished_at)
VALUES (1, 'idle', '', 0, 0);
