DROP INDEX IF EXISTS idx_branch_op_results_repo;
DROP TABLE IF EXISTS branch_op_results;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:b1f8ad40b174a845ad0e7da0a2e14a1d9749d3035c80efea4cc508cffa539262';
