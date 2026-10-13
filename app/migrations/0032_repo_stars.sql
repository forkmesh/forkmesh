-- Repo stars: which accounts have starred which repo. Keyed by blind indexes
-- (same convention as profile_follows) so the count/membership check never
-- needs to decrypt anything.
CREATE TABLE IF NOT EXISTS repo_stars (
    repo_bi TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, account_bi)
);

CREATE INDEX IF NOT EXISTS idx_repo_stars_repo
    ON repo_stars(repo_bi, created_at);
CREATE INDEX IF NOT EXISTS idx_repo_stars_account
    ON repo_stars(account_bi, created_at);
