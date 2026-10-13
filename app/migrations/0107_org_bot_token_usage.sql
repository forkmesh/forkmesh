-- Metadata-only organization bot-token usage history. Secrets, request
-- bodies, query strings, IP addresses, and user-agent values never enter this
-- table. Organization admins receive a short preview; the full append-only
-- table is visible in the platform administration browser.
CREATE TABLE IF NOT EXISTS org_bot_token_usage (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  token_id   TEXT NOT NULL,
  org_bi     TEXT NOT NULL,
  provider   TEXT NOT NULL
               CHECK (provider IN ('codex', 'claude-code')),
  action     TEXT NOT NULL CHECK (length(action) BETWEEN 1 AND 120),
  method     TEXT NOT NULL CHECK (method IN ('GET', 'POST', 'PUT', 'PATCH', 'DELETE')),
  used_at    INTEGER NOT NULL CHECK (used_at >= 0)
);
CREATE INDEX IF NOT EXISTS idx_org_bot_token_usage_org_time
  ON org_bot_token_usage(org_bi, used_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_org_bot_token_usage_token_time
  ON org_bot_token_usage(token_id, used_at DESC, id DESC);
