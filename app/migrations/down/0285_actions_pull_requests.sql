DROP TABLE IF EXISTS repo_action_trusted_authors;
DROP INDEX IF EXISTS idx_repo_action_jobs_pull;
ALTER TABLE repo_action_jobs DROP COLUMN base_oid;
ALTER TABLE repo_action_jobs DROP COLUMN base_branch;
ALTER TABLE repo_action_jobs DROP COLUMN approved_by;
ALTER TABLE repo_action_jobs DROP COLUMN approval;
ALTER TABLE repo_action_jobs DROP COLUMN pull_number;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:77e8e7ee1d331dc3ee91089720e9d4a54f90ec2ce1599cad171e3020f61e1e88';
