DROP INDEX IF EXISTS idx_users_early_ios_tester;

ALTER TABLE users DROP COLUMN early_ios_tester;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:2fd181c40f5a3e77452e7a5aca495f14385fcd13c3251b3aea1fef3d9ec8f274';
