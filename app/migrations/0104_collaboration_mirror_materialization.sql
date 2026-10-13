-- Let integrity-approved online mirrors materialize pull requests and
-- discussions immediately, matching the existing issue path. The encrypted
-- relay row remains until the source-of-truth node commits it.
ALTER TABLE pull_inbox
  ADD COLUMN claimed_by_bi TEXT NOT NULL DEFAULT '';
ALTER TABLE pull_inbox
  ADD COLUMN claim_expires_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE pull_inbox
  ADD COLUMN mirrored_by_bi TEXT NOT NULL DEFAULT '';
ALTER TABLE pull_inbox
  ADD COLUMN mirrored_at INTEGER NOT NULL DEFAULT 0;
CREATE INDEX IF NOT EXISTS idx_pull_inbox_mirror_pending
  ON pull_inbox(repo_bi, mirrored_at, claimed_by_bi, claim_expires_at);

ALTER TABLE discussion_inbox
  ADD COLUMN claimed_by_bi TEXT NOT NULL DEFAULT '';
ALTER TABLE discussion_inbox
  ADD COLUMN claim_expires_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE discussion_inbox
  ADD COLUMN mirrored_by_bi TEXT NOT NULL DEFAULT '';
ALTER TABLE discussion_inbox
  ADD COLUMN mirrored_at INTEGER NOT NULL DEFAULT 0;
CREATE INDEX IF NOT EXISTS idx_discussion_inbox_mirror_pending
  ON discussion_inbox(repo_bi, mirrored_at, claimed_by_bi, claim_expires_at);
