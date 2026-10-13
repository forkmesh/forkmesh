-- Edge request-policy rules. Patterns are operator-managed data: source and
-- migrations define only the supported match types and contain no live rule.
CREATE TABLE IF NOT EXISTS request_block_rules (
    rule_id TEXT PRIMARY KEY,
    match_type TEXT NOT NULL CHECK (match_type IN (
        'user_agent_keyword', 'ip', 'cidr', 'ip_range')),
    pattern TEXT NOT NULL CHECK (length(trim(pattern)) > 0),
    enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
    priority INTEGER NOT NULL DEFAULT 100,
    note TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL CHECK (created_at > 0),
    updated_at INTEGER NOT NULL CHECK (updated_at > 0),
    UNIQUE (match_type, pattern)
);

CREATE INDEX IF NOT EXISTS idx_request_block_rules_active
    ON request_block_rules(enabled, priority DESC, created_at, rule_id);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:51ee0907c7f75869d3761cbab54303139b7cd46a055a05f068742e0fd1b25105',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
