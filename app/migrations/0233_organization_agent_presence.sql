CREATE TABLE IF NOT EXISTS organization_agent_presence (org_bi TEXT NOT NULL, account_bi TEXT NOT NULL, session_id TEXT NOT NULL, data TEXT NOT NULL, live INTEGER NOT NULL DEFAULT 0 CHECK (live IN (0, 1)), updated_at INTEGER NOT NULL, PRIMARY KEY (org_bi, account_bi, session_id));

CREATE INDEX IF NOT EXISTS idx_organization_agent_presence_org
ON organization_agent_presence(org_bi, updated_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:7934721f688df24fdde16249a787a69c2e46a03168a799b0274039d3af643b8b',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
