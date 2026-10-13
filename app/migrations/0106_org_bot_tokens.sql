-- Revocable organization bot credentials. The plaintext token is returned
-- once; D1 retains only a keyed blind index plus encrypted display metadata.
CREATE TABLE IF NOT EXISTS org_bot_tokens (
  token_id       TEXT PRIMARY KEY,
  org_bi         TEXT NOT NULL,
  secret_bi      TEXT NOT NULL UNIQUE,
  provider       TEXT NOT NULL
                   CHECK (provider IN ('codex', 'claude-code')),
  data           TEXT NOT NULL,
  created_by_bi  TEXT NOT NULL,
  created_at     INTEGER NOT NULL CHECK (created_at >= 0),
  last_used_at   INTEGER NOT NULL DEFAULT 0 CHECK (last_used_at >= 0),
  expires_at     INTEGER NOT NULL DEFAULT 0 CHECK (expires_at >= 0),
  revoked_at     INTEGER NOT NULL DEFAULT 0 CHECK (revoked_at >= 0)
);
CREATE INDEX IF NOT EXISTS idx_org_bot_tokens_org
  ON org_bot_tokens(org_bi, revoked_at, created_at DESC);
