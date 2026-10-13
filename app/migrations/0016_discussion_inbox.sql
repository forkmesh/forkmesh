-- ForkMesh D1 migration 0016 - discussion inbox.

CREATE TABLE IF NOT EXISTS discussion_inbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    submitter_bi TEXT
);

CREATE INDEX IF NOT EXISTS idx_discussion_inbox_repo ON discussion_inbox(repo_bi);
