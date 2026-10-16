DROP INDEX IF EXISTS idx_org_bot_token_usage_token_time;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:31e698fa0f738ce3ed690feccaaabf062f96e1b82f8d56b3f98886a813b7d7c0',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
