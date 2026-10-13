DROP INDEX IF EXISTS idx_workflow_credentials_owner;
DROP TABLE IF EXISTS workflow_credentials;
DROP INDEX IF EXISTS idx_workflow_runs_owner;
DROP INDEX IF EXISTS idx_workflow_runs_workflow;
DROP TABLE IF EXISTS workflow_runs;
DROP INDEX IF EXISTS idx_workflows_schedule;
DROP INDEX IF EXISTS idx_workflows_owner;
DROP TABLE IF EXISTS workflows;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:f9464ffd954065beb046b07ae39575394449ace82c03a8ee940a9be08907d6c6';
