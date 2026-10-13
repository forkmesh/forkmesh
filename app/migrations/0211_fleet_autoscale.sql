CREATE TABLE IF NOT EXISTS autoscale_settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    doc TEXT NOT NULL,
    signed_at INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS autoscale_state (
    pool TEXT PRIMARY KEY,
    data TEXT NOT NULL,
    updated_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS autoscale_nodes (
    name TEXT PRIMARY KEY,
    pool TEXT NOT NULL,
    label TEXT NOT NULL UNIQUE,
    instance_id TEXT,
    created_by TEXT NOT NULL,
    state TEXT NOT NULL,
    state_since_ms INTEGER NOT NULL,
    public_ipv4 TEXT,
    wg_ip TEXT,
    wg_pubkey TEXT,
    ids_pubkey TEXT,
    runner_pubkey TEXT,
    token_hash TEXT,
    token_expires_ms INTEGER,
    token_used_ms INTEGER,
    rev TEXT,
    hourly_cents REAL NOT NULL DEFAULT 0,
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL,
    detail TEXT
);

CREATE TABLE IF NOT EXISTS autoscale_lease (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    holder TEXT NOT NULL,
    expires_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS autoscale_audit (
    id TEXT PRIMARY KEY,
    ts INTEGER NOT NULL,
    pool TEXT NOT NULL,
    event TEXT NOT NULL,
    node TEXT,
    ok INTEGER NOT NULL,
    detail TEXT
);
CREATE INDEX IF NOT EXISTS idx_autoscale_audit_ts ON autoscale_audit(ts);

CREATE TABLE IF NOT EXISTS autoscale_commands (
    id TEXT PRIMARY KEY,
    created_ms INTEGER NOT NULL,
    pool TEXT NOT NULL,
    direction TEXT NOT NULL,
    node TEXT,
    reason TEXT,
    done_ms INTEGER,
    result TEXT
);

CREATE TABLE IF NOT EXISTS autoscale_nonces (
    nonce TEXT PRIMARY KEY,
    expires_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS host_metrics (
    node_id TEXT PRIMARY KEY,
    ms INTEGER NOT NULL,
    data TEXT NOT NULL
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:c98214c4a3dea0015082c2475552687a0673b402d4ab79592ce1546ec080b769',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
