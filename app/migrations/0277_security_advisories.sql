CREATE TABLE IF NOT EXISTS security_advisories (
    id TEXT PRIMARY KEY,
    repo_bi TEXT NOT NULL,
    state TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    published_at INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    public_data TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_security_advisories_repo
    ON security_advisories(repo_bi, state, updated_at);

CREATE INDEX IF NOT EXISTS idx_security_advisories_published
    ON security_advisories(state, published_at);

CREATE TABLE IF NOT EXISTS security_advisory_members (
    advisory_id TEXT NOT NULL,
    repo_bi TEXT NOT NULL,
    member_bi TEXT NOT NULL,
    role TEXT NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (advisory_id, member_bi)
);

CREATE INDEX IF NOT EXISTS idx_security_advisory_members_repo
    ON security_advisory_members(repo_bi, member_bi);

CREATE TABLE IF NOT EXISTS security_advisory_comments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    advisory_id TEXT NOT NULL,
    repo_bi TEXT NOT NULL,
    ts INTEGER NOT NULL,
    data TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_security_advisory_comments_advisory
    ON security_advisory_comments(advisory_id, id);

CREATE TABLE IF NOT EXISTS security_advisory_credits (
    advisory_id TEXT NOT NULL,
    repo_bi TEXT NOT NULL,
    user_bi TEXT NOT NULL,
    state TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (advisory_id, user_bi)
);

CREATE INDEX IF NOT EXISTS idx_security_advisory_credits_user
    ON security_advisory_credits(user_bi, state);

CREATE TABLE IF NOT EXISTS security_advisory_settings (
    repo_bi TEXT PRIMARY KEY,
    reporting INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:c42d16ef4a1708560657263b86ef151172e3f35dae0b14f9f8ae36fcc7562e21',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
