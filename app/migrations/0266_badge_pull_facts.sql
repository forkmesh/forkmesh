CREATE TABLE IF NOT EXISTS badge_pull_facts (
    repo_bi TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('pull', 'issue')),
    number INTEGER NOT NULL CHECK (number > 0),
    author_key TEXT NOT NULL DEFAULT '',
    closer_key TEXT NOT NULL DEFAULT '',
    closer_bi TEXT NOT NULL DEFAULT '',
    opened_at INTEGER NOT NULL DEFAULT 0 CHECK (opened_at >= 0),
    closed_at INTEGER NOT NULL DEFAULT 0 CHECK (closed_at >= 0),
    merged INTEGER NOT NULL DEFAULT 0 CHECK (merged IN (0, 1)),
    reviewed INTEGER NOT NULL DEFAULT 0 CHECK (reviewed IN (0, 1)),
    review_waived INTEGER NOT NULL DEFAULT 0
        CHECK (review_waived IN (0, 1)),
    updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0),
    PRIMARY KEY (repo_bi, kind, number));

CREATE INDEX IF NOT EXISTS idx_badge_pull_facts_author ON badge_pull_facts(author_key);

CREATE TABLE IF NOT EXISTS badge_coauthor_facts (
    repo_bi TEXT NOT NULL,
    number INTEGER NOT NULL CHECK (number > 0),
    account_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    PRIMARY KEY (repo_bi, number, account_bi));

CREATE TABLE IF NOT EXISTS badge_repo_sweeps (
    repo_bi TEXT PRIMARY KEY,
    state_ts INTEGER NOT NULL DEFAULT 0 CHECK (state_ts >= 0),
    swept_at INTEGER NOT NULL DEFAULT 0 CHECK (swept_at >= 0));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:c001b5e1691b7e406a0f778b87e193c27dafa246e70913476c444078966d5781',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
