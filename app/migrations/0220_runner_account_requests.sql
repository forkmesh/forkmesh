CREATE TABLE IF NOT EXISTS runner_account_requests (
  id TEXT PRIMARY KEY,
  runner TEXT NOT NULL,
  provider TEXT NOT NULL,
  action TEXT NOT NULL CHECK (action IN ('activate','remove')),
  account TEXT NOT NULL,
  state TEXT NOT NULL DEFAULT 'pending'
      CHECK (state IN ('pending','done','failed')),
  created_by TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  message TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_runner_account_requests_runner
  ON runner_account_requests(runner, state, created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:14342a074189c5e6876d7e475cc0d7f1fdad578dc771aa89bad753770e3f5f42',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
