-- Let any authorized online repository mirror materialize pending issues while
-- ensuring exactly one device owns a row at a time. Claims are opaque blind
-- indexes of registered signing keys and expire automatically after a bounded
-- lease, so an interrupted mirror cannot strand an issue.
ALTER TABLE issue_inbox
  ADD COLUMN claimed_by_bi TEXT NOT NULL DEFAULT '';
ALTER TABLE issue_inbox
  ADD COLUMN claim_expires_at INTEGER NOT NULL DEFAULT 0;
CREATE INDEX IF NOT EXISTS idx_issue_inbox_claim
  ON issue_inbox(repo_bi, claim_expires_at, claimed_by_bi);
