ALTER TABLE issue_inbox
ADD COLUMN mirrored_by_bi TEXT NOT NULL DEFAULT '';

ALTER TABLE issue_inbox
ADD COLUMN mirrored_at INTEGER NOT NULL DEFAULT 0;

CREATE INDEX IF NOT EXISTS idx_issue_inbox_mirror_pending
ON issue_inbox(repo_bi, mirrored_at, claimed_by_bi, claim_expires_at);
