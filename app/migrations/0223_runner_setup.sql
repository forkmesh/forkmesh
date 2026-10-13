CREATE TABLE IF NOT EXISTS runner_setup_tokens (
  id TEXT PRIMARY KEY,
  token_hash TEXT NOT NULL UNIQUE,
  owner TEXT NOT NULL,
  runner TEXT NOT NULL,
  via TEXT NOT NULL DEFAULT 'command',
  state TEXT NOT NULL DEFAULT 'pending'
      CHECK (state IN ('pending','used','revoked')),
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  used_at INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_runner_setup_tokens_owner
  ON runner_setup_tokens(owner, created_at);

CREATE TABLE IF NOT EXISTS runner_ssh_setups (
  id TEXT PRIMARY KEY,
  owner TEXT NOT NULL,
  host TEXT NOT NULL,
  address TEXT NOT NULL,
  port INTEGER NOT NULL,
  host_keys TEXT NOT NULL DEFAULT '[]',
  fingerprint TEXT NOT NULL DEFAULT '',
  runner TEXT NOT NULL DEFAULT '',
  token_id TEXT NOT NULL DEFAULT '',
  state TEXT NOT NULL DEFAULT 'probed'
      CHECK (state IN ('probed','running','succeeded','failed')),
  message TEXT NOT NULL DEFAULT '',
  log TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_runner_ssh_setups_owner
  ON runner_ssh_setups(owner, created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:cbca726cb191ebd604560836529151bd85d06b311bdd3c713e4b341d9950ccef',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
