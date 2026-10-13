DROP INDEX IF EXISTS idx_repo_action_jobs_status;
DROP INDEX IF EXISTS idx_repo_action_jobs_repo;
DROP TABLE IF EXISTS repo_action_jobs;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:871f73bd739ff6ceddd9d84cfc5b1b76c8c7f36210843fc58a22e0fe91e206a7';
