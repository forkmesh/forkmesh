CREATE TABLE IF NOT EXISTS pull_submission_receipts (
  repo_bi TEXT NOT NULL,
  submission_bi TEXT NOT NULL,
  data TEXT NOT NULL,
  PRIMARY KEY (repo_bi, submission_bi)
);

ALTER TABLE pull_inbox ADD COLUMN submission_bi TEXT NOT NULL DEFAULT '';
CREATE UNIQUE INDEX IF NOT EXISTS idx_pull_inbox_submission
  ON pull_inbox(repo_bi, submission_bi)
  WHERE submission_bi <> '';

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:c6b023b37b488e326ace84d1d1ceead6c8e35abdeda93d24a746ab3182007690',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
