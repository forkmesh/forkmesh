CREATE INDEX IF NOT EXISTS idx_org_bot_token_usage_token_time
    ON org_bot_token_usage(token_id, used_at DESC, id DESC);

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:31e698fa0f738ce3ed690feccaaabf062f96e1b82f8d56b3f98886a813b7d7c0';
