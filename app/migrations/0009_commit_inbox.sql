-- ForkMesh D1 migration 0009 — commit-comment inbox.
-- Holds signed per-commit comments submitted by nodes without write access to a
-- repo; the owner drains and commits them into commits/<sha>/. The worker also
-- creates this lazily (ensure_schema in src/entry.py); this migration keeps the
-- file-based schema in sync. Each row's `data` is an encrypted AES-GCM blob
-- (envelope), like the issue/pull inboxes — no plaintext user content here.

CREATE TABLE IF NOT EXISTS commit_inbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_bi TEXT NOT NULL,      -- blind index of owner/repo
    data TEXT NOT NULL          -- encrypted {sha, comment, submitter, submittedAt}
);

CREATE INDEX IF NOT EXISTS idx_commit_inbox_repo ON commit_inbox(repo_bi);
