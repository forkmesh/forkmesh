CREATE TABLE IF NOT EXISTS instance_launches (
    id TEXT PRIMARY KEY,
    client_key TEXT NOT NULL,
    account TEXT NOT NULL DEFAULT '',
    project TEXT NOT NULL,
    domain TEXT NOT NULL,
    owner TEXT NOT NULL,
    owner_type TEXT NOT NULL DEFAULT 'org'
        CHECK (owner_type IN ('org','user')),
    admin TEXT NOT NULL,
    address TEXT NOT NULL,
    port INTEGER NOT NULL,
    host_key TEXT NOT NULL DEFAULT '{}',
    state TEXT NOT NULL DEFAULT 'running'
        CHECK (state IN ('running','succeeded','failed')),
    steps TEXT NOT NULL DEFAULT '[]',
    message TEXT NOT NULL DEFAULT '',
    log TEXT NOT NULL DEFAULT '',
    url TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_instance_launches_client
    ON instance_launches(client_key, created_at);
CREATE INDEX IF NOT EXISTS idx_instance_launches_state
    ON instance_launches(state, updated_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:d2437168fb4d6773c730ffe414f7b28c83adef80e30f460a232d850a9d509746',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
