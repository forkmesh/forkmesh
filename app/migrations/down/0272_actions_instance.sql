DROP TABLE IF EXISTS repo_action_logs;

DROP TABLE IF EXISTS repo_action_secrets;

DROP TABLE IF EXISTS repo_action_workflows;

DROP INDEX IF EXISTS idx_repo_action_jobs_token;

ALTER TABLE runner_nodes DROP COLUMN actions_trusted;

ALTER TABLE repo_action_jobs DROP COLUMN pushed_commit;

ALTER TABLE repo_action_jobs DROP COLUMN trusted;

ALTER TABLE repo_action_jobs DROP COLUMN token_hash;

ALTER TABLE repo_action_jobs DROP COLUMN payload;

ALTER TABLE repo_action_jobs DROP COLUMN before_oid;

ALTER TABLE repo_action_jobs DROP COLUMN actor;

ALTER TABLE repo_action_jobs DROP COLUMN ref;

ALTER TABLE repo_action_jobs DROP COLUMN workflow;

ALTER TABLE repo_action_jobs DROP COLUMN event;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:bd23d448215d983ffe1f306a258fa0c4918fb74df8b099c19f7632986f37a6b5';
