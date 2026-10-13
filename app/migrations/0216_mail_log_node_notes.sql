CREATE TABLE IF NOT EXISTS mail_log_node_notes (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        created_at INTEGER NOT NULL,
        node TEXT NOT NULL DEFAULT '' CHECK (length(node) <= 64),
        data TEXT NOT NULL CHECK (length(data) <= 4096));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:f08fee239ae7fc31c4a5d1d81df31f0d7ed97e6dd144d2d61f520fd2846a9884',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
