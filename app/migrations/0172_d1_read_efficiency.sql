-- Keep the three highest-leverage D1 reads on indexed paths. The retention
-- sweep filters globally by timestamp, while roster readers order/filter on
-- plaintext operational columns that were previously unindexed.
CREATE INDEX IF NOT EXISTS idx_org_bot_token_usage_used_at
    ON org_bot_token_usage(used_at);

CREATE INDEX IF NOT EXISTS idx_users_username
    ON users(username);

CREATE INDEX IF NOT EXISTS idx_users_admin
    ON users(is_admin, user_bi);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:c9d21b0179551331b43566a7861268ac90dbaa73d6c4a051c55dea0744fcfd14',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
