-- Public direct-HTTPS mirror discovery and signed forkmesh/forkmesh health.
-- Rows contain routing/attestation metadata only, never repository bytes,
-- private repository names, wallet material, or node private keys.

CREATE TABLE IF NOT EXISTS mirror_https_endpoints (
    node_bi TEXT PRIMARY KEY,
    node_name TEXT NOT NULL,
    base_url TEXT NOT NULL,
    public_key TEXT NOT NULL,
    registration_sig TEXT NOT NULL,
    issued_at INTEGER NOT NULL,
    checked_at INTEGER NOT NULL DEFAULT 0,
    latency_ms INTEGER NOT NULL DEFAULT 0,
    region TEXT,
    healthy INTEGER NOT NULL DEFAULT 0,
    integrity TEXT NOT NULL DEFAULT 'unknown',
    abuse_blocked INTEGER NOT NULL DEFAULT 0,
    health_sig TEXT,
    forkmesh_verified_at INTEGER NOT NULL DEFAULT 0,
    forkmesh_refs_sha256 TEXT NOT NULL DEFAULT '',
    forkmesh_operations_sha256 TEXT NOT NULL DEFAULT '',
    forkmesh_active INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_mirror_https_endpoint_name
    ON mirror_https_endpoints(node_name);
CREATE INDEX IF NOT EXISTS idx_mirror_https_endpoint_health
    ON mirror_https_endpoints(healthy, checked_at);

CREATE TABLE IF NOT EXISTS edge_route_cursor (
    repo_bi TEXT PRIMARY KEY,
    cursor INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL
);
