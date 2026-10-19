CREATE TABLE IF NOT EXISTS pull_review_ledger (
    repo_bi TEXT NOT NULL,
    pull_number INTEGER NOT NULL,
    account TEXT NOT NULL,
    state TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, pull_number, account)
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:b757172124ea52f42581b43e267268ba4e227b53eb8dffa9b561f308dfb3c1b9',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
