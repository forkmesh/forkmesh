DROP TABLE IF EXISTS repo_action_trusted_authors;
DROP INDEX IF EXISTS idx_repo_action_jobs_pull;
ALTER TABLE repo_action_jobs DROP COLUMN base_oid;
ALTER TABLE repo_action_jobs DROP COLUMN base_branch;
ALTER TABLE repo_action_jobs DROP COLUMN approved_by;
ALTER TABLE repo_action_jobs DROP COLUMN approval;
ALTER TABLE repo_action_jobs DROP COLUMN pull_number;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:a375486c83110cb3e4acdc510331f8628dab47ce801bd87358de4c4952cdcb29';
