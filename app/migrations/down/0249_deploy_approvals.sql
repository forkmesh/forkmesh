DROP TABLE IF EXISTS deploy_approval_nodes;
DROP INDEX IF EXISTS idx_deploy_approvals_status;
DROP TABLE IF EXISTS deploy_approvals;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:890825e44baaf83bf18831236cd246e08beaceb6640afc829301a6a8e90e598a';
