CREATE TABLE IF NOT EXISTS agent_note_queue (
    entry_id TEXT PRIMARY KEY,
    status TEXT NOT NULL DEFAULT 'pending',
    created_at INTEGER NOT NULL DEFAULT 0,
    reviewed_at INTEGER NOT NULL DEFAULT 0,
    note_id TEXT NOT NULL DEFAULT '',
    data TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_agent_note_queue_status
ON agent_note_queue(status, created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:a0fa4a2a85f642c250787c6ae41e6acba3816daa3b869b1cd9f76d57646a4a58',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
