CREATE TABLE IF NOT EXISTS instance_registrations (
    domain TEXT PRIMARY KEY,
    public_key TEXT NOT NULL,
    world_url TEXT NOT NULL DEFAULT '',
    announced_at INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS instance_register_attempts (
    client_key TEXT NOT NULL,
    domain TEXT NOT NULL,
    at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_instance_register_attempts_at
    ON instance_register_attempts(at);
CREATE INDEX IF NOT EXISTS idx_instance_register_attempts_client
    ON instance_register_attempts(client_key, at);
CREATE INDEX IF NOT EXISTS idx_instance_register_attempts_domain
    ON instance_register_attempts(domain, at);
