DROP INDEX IF EXISTS idx_quarantined_users_discord;

ALTER TABLE quarantined_users DROP COLUMN discord_bi;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:4db7c6029ab4ec3699ef7eaa2355a198ee9e19550e6fee11e973a87c42653dd0';
