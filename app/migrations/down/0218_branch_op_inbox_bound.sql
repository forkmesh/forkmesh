DROP TRIGGER IF EXISTS trg_branch_op_inbox_repo_bound;

CREATE TRIGGER IF NOT EXISTS trg_branch_op_inbox_repo_bound
BEFORE INSERT ON branch_op_inbox
WHEN (SELECT COUNT(*) FROM branch_op_inbox
       WHERE repo_bi=NEW.repo_bi) >= 64
BEGIN
  SELECT RAISE(ABORT, 'branch_op_inbox_repo_limit');
END;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:116b6b44beb90734cac5bc4fc6d15bab36e992db47be36aeed31cecee7427918';
