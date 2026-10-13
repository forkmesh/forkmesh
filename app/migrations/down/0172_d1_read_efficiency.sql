DROP INDEX IF EXISTS idx_org_bot_token_usage_used_at;
DROP INDEX IF EXISTS idx_users_username;
DROP INDEX IF EXISTS idx_users_admin;
DELETE FROM schema_meta
 WHERE k = 'schema_fingerprint:c9d21b0179551331b43566a7861268ac90dbaa73d6c4a051c55dea0744fcfd14';
