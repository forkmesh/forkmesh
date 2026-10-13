-- Fork edges: which repositories are forks of which other repository, so the
-- parent's repo page can list the people who forked it.
--
-- The edge is not an independent claim: it is indexed from the forking node's
-- own owner-signed catalog record (`forkedFrom`), which is only accepted for a
-- repository that node already publishes. fork_owner/fork_name are the same
-- public strings the catalog publishes for a public repository, stored in
-- plaintext (like profile_follows' names) so listing forks is one indexed read
-- rather than a decrypt pass over every catalog row.
CREATE TABLE IF NOT EXISTS repo_forks (
    parent_bi TEXT NOT NULL,
    fork_bi TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    fork_owner TEXT NOT NULL,
    fork_name TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (parent_bi, fork_bi)
);
CREATE INDEX IF NOT EXISTS idx_repo_forks_parent ON repo_forks(parent_bi, created_at);
CREATE INDEX IF NOT EXISTS idx_repo_forks_fork ON repo_forks(fork_bi);
CREATE INDEX IF NOT EXISTS idx_repo_forks_account ON repo_forks(account_bi);
