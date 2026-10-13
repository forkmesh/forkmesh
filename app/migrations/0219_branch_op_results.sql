CREATE TABLE IF NOT EXISTS branch_op_results (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    reported_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_branch_op_results_repo
    ON branch_op_results(repo_bi, id);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:b1f8ad40b174a845ad0e7da0a2e14a1d9749d3035c80efea4cc508cffa539262',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
