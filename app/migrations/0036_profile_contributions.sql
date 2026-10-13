-- Bounded, privacy-preserving read model for verified profile contribution
-- snapshots. Human-readable repository labels remain inside encrypted data;
-- raw Git author names and email addresses are never stored.
CREATE TABLE IF NOT EXISTS profile_contribution_receipts (
    generation_bi TEXT PRIMARY KEY,
    snapshot_hash TEXT NOT NULL,
    source_account_bi TEXT NOT NULL,
    source_repo_bi TEXT NOT NULL,
    captured_at INTEGER NOT NULL,
    head TEXT NOT NULL,
    day_rows INTEGER NOT NULL,
    language_rows INTEGER NOT NULL,
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS profile_contribution_projects (
    source_repo_bi TEXT PRIMARY KEY,
    source_account_bi TEXT NOT NULL,
    owner_user_bi TEXT NOT NULL,
    project_bi TEXT NOT NULL,
    first_public_day TEXT NOT NULL,
    is_public INTEGER NOT NULL DEFAULT 1,
    active_generation_bi TEXT,
    captured_at INTEGER NOT NULL DEFAULT 0,
    verified_from TEXT,
    data TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS profile_contribution_days (
    generation_bi TEXT NOT NULL,
    subject_user_bi TEXT NOT NULL,
    source_account_bi TEXT NOT NULL,
    source_repo_bi TEXT NOT NULL,
    project_bi TEXT NOT NULL,
    day TEXT NOT NULL,
    commits INTEGER NOT NULL DEFAULT 0,
    issues INTEGER NOT NULL DEFAULT 0,
    pulls INTEGER NOT NULL DEFAULT 0,
    reviews INTEGER NOT NULL DEFAULT 0,
    captured_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (generation_bi, subject_user_bi, source_repo_bi, day)
);

CREATE TABLE IF NOT EXISTS profile_contribution_languages (
    generation_bi TEXT NOT NULL,
    owner_user_bi TEXT NOT NULL,
    source_repo_bi TEXT NOT NULL,
    project_bi TEXT NOT NULL,
    language TEXT NOT NULL,
    bytes INTEGER NOT NULL,
    files INTEGER NOT NULL,
    captured_at INTEGER NOT NULL,
    PRIMARY KEY (generation_bi, owner_user_bi, source_repo_bi, language)
);

CREATE INDEX IF NOT EXISTS idx_profile_contribution_projects_owner_public
    ON profile_contribution_projects(owner_user_bi, is_public);
CREATE INDEX IF NOT EXISTS idx_profile_contribution_projects_project_public
    ON profile_contribution_projects(project_bi, is_public);
CREATE INDEX IF NOT EXISTS idx_profile_contribution_days_subject_day
    ON profile_contribution_days(subject_user_bi, day);
CREATE INDEX IF NOT EXISTS idx_profile_contribution_days_repo_generation
    ON profile_contribution_days(source_repo_bi, generation_bi);
CREATE INDEX IF NOT EXISTS idx_profile_contribution_languages_owner_generation
    ON profile_contribution_languages(owner_user_bi, generation_bi);
