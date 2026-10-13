DROP INDEX IF EXISTS idx_runner_ssh_setups_owner;
DROP TABLE IF EXISTS runner_ssh_setups;
DROP INDEX IF EXISTS idx_runner_setup_tokens_owner;
DROP TABLE IF EXISTS runner_setup_tokens;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:6cf7f58d6e875f703d9f602e09248464a1e98073f668c2ebd646caafe1fdf770';
