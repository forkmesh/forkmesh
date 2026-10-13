CREATE TABLE IF NOT EXISTS deploy_approvals (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        request_id TEXT NOT NULL UNIQUE,
        kind TEXT NOT NULL,
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        target TEXT NOT NULL,
        expected_base TEXT NOT NULL DEFAULT '',
        expected_head TEXT NOT NULL DEFAULT '',
        base_before TEXT NOT NULL,
        head TEXT NOT NULL,
        base_after TEXT NOT NULL,
        public_key TEXT NOT NULL,
        signature TEXT NOT NULL,
        signed_at INTEGER NOT NULL,
        actor TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending',
        message TEXT NOT NULL DEFAULT '',
        announced INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_deploy_approvals_status
    ON deploy_approvals(status, id);
CREATE TABLE IF NOT EXISTS deploy_approval_nodes (
        approval_id INTEGER NOT NULL,
        node TEXT NOT NULL,
        status TEXT NOT NULL,
        message TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (approval_id, node));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:890825e44baaf83bf18831236cd246e08beaceb6640afc829301a6a8e90e598a',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
