DROP TRIGGER IF EXISTS trg_branch_op_inbox_repo_bound;
DROP INDEX IF EXISTS idx_branch_op_inbox_repo;
DROP TABLE IF EXISTS branch_op_inbox;
DELETE FROM schema_meta
 WHERE k = 'schema_fingerprint:9c8c32bcbc1af75a6fc02c1aafc5f40f244cd6c482fda44f6daece028fd38d1b';
