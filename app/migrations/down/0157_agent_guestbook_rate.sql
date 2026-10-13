DROP INDEX IF EXISTS idx_agent_guestbook_rate_window;
DROP TABLE IF EXISTS agent_guestbook_rate;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:4273f4cba21862087cf50167d9f9e14bd722b7205d79aa6e39978e20d18bf4e7';
