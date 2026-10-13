-- GitHub/GitLab metadata imports remain separate from the live mirror catalog.
-- ``data`` columns are encrypted by the Worker; private listings and invitation
-- addresses therefore never appear as plaintext in D1.

CREATE TABLE IF NOT EXISTS repository_imports (
    id TEXT PRIMARY KEY,
    provider TEXT NOT NULL,
    external_id TEXT NOT NULL,
    owner_bi TEXT NOT NULL,
    is_private INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'external_repository',
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_repository_imports_provider_id
    ON repository_imports(provider, external_id);
CREATE INDEX IF NOT EXISTS idx_repository_imports_public
    ON repository_imports(is_private, updated_at);
CREATE INDEX IF NOT EXISTS idx_repository_imports_owner
    ON repository_imports(owner_bi, updated_at);

CREATE TABLE IF NOT EXISTS repository_mirror_volunteers (
    repo_id TEXT NOT NULL,
    operator_bi TEXT NOT NULL,
    node_label TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'requested',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (repo_id, operator_bi)
);
CREATE INDEX IF NOT EXISTS idx_repository_mirror_volunteers_status
    ON repository_mirror_volunteers(repo_id, status, updated_at);

CREATE TABLE IF NOT EXISTS contributor_invitations (
    id TEXT PRIMARY KEY,
    repo_id TEXT NOT NULL,
    inviter_bi TEXT NOT NULL,
    email_bi TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    sent_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_contributor_invitations_repo
    ON contributor_invitations(repo_id, created_at);
CREATE INDEX IF NOT EXISTS idx_contributor_invitations_recipient
    ON contributor_invitations(email_bi, created_at);

CREATE TABLE IF NOT EXISTS contributor_invitation_rate (
    inviter_bi TEXT NOT NULL,
    day_bucket INTEGER NOT NULL,
    repo_id TEXT NOT NULL,
    sent_count INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (inviter_bi, day_bucket, repo_id)
);

CREATE TABLE IF NOT EXISTS contributor_invitation_optouts (
    email_bi TEXT PRIMARY KEY,
    opted_out_at INTEGER NOT NULL,
    source_invitation_id TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS contributor_invitation_abuse (
    id TEXT PRIMARY KEY,
    invitation_id TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'open'
);
CREATE INDEX IF NOT EXISTS idx_contributor_invitation_abuse_invitation
    ON contributor_invitation_abuse(invitation_id, created_at);

CREATE TABLE IF NOT EXISTS repository_logo_suggestions (
    id TEXT PRIMARY KEY,
    repo_id TEXT NOT NULL,
    proposer_bi TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    official INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    reviewed_at INTEGER NOT NULL DEFAULT 0,
    reviewed_by_bi TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_repository_logo_suggestions_repo
    ON repository_logo_suggestions(repo_id, status, created_at);
CREATE INDEX IF NOT EXISTS idx_repository_logo_suggestions_proposer
    ON repository_logo_suggestions(repo_id, proposer_bi, status, created_at);
CREATE UNIQUE INDEX IF NOT EXISTS idx_repository_logo_official
    ON repository_logo_suggestions(repo_id) WHERE official=1;
