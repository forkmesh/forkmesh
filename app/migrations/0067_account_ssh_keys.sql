-- User-managed SSH public keys for the independently operated Git SSH gateway.
-- Raw public-key lines and user labels are encrypted in `data`; account/key
-- lookups use keyed blind indexes. Private keys never enter ForkMesh.
CREATE TABLE IF NOT EXISTS account_ssh_keys (
  key_id TEXT PRIMARY KEY,
  account_bi TEXT NOT NULL,
  key_bi TEXT NOT NULL UNIQUE,
  key_type TEXT NOT NULL,
  fingerprint TEXT NOT NULL,
  data TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  last_used_at INTEGER NOT NULL DEFAULT 0,
  revoked_at INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_account_ssh_keys_account
  ON account_ssh_keys(account_bi, revoked_at, created_at);
