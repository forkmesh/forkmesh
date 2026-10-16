CREATE TABLE IF NOT EXISTS node_uptime_totals (
    node_key TEXT PRIMARY KEY,
    minutes INTEGER NOT NULL DEFAULT 0 CHECK (minutes >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:a5893d085c0a40f89092da1e1b5847e10e39915039ae754ef33df3de67b58681',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
