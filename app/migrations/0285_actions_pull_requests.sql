ALTER TABLE repo_action_jobs ADD COLUMN pull_number INTEGER NOT NULL DEFAULT 0;

ALTER TABLE repo_action_jobs ADD COLUMN approval TEXT NOT NULL DEFAULT '';

ALTER TABLE repo_action_jobs ADD COLUMN approved_by TEXT NOT NULL DEFAULT '';

ALTER TABLE repo_action_jobs ADD COLUMN base_branch TEXT NOT NULL DEFAULT '';

ALTER TABLE repo_action_jobs ADD COLUMN base_oid TEXT NOT NULL DEFAULT '';

CREATE INDEX IF NOT EXISTS idx_repo_action_jobs_pull
    ON repo_action_jobs(owner, repo, pull_number, id) WHERE pull_number > 0;

CREATE TABLE IF NOT EXISTS repo_action_trusted_authors (
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        account TEXT NOT NULL,
        added_by TEXT NOT NULL DEFAULT '',
        added_at INTEGER NOT NULL,
        PRIMARY KEY (owner, repo, account));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:77e8e7ee1d331dc3ee91089720e9d4a54f90ec2ce1599cad171e3020f61e1e88',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
