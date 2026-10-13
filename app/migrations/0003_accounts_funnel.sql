-- ForkMesh D1 migration 0003 — staged signup funnel.
-- The accounts table now backs a multi-step funnel (reserve node name → donate →
-- set email+password) instead of a one-shot signup. The encrypted `data` blob
-- gains funnel fields (status, donation_address, donation_reference,
-- donation_confirmed, email, pass_hash) with no schema change.
--
-- This migration only adds the email blind index, so a user can log in by email
-- (not just node name) without decrypting every row. The column is added to the
-- existing table for already-deployed databases; fresh databases get it from
-- ensure_schema() in src/entry.py. SQLite has no "ADD COLUMN IF NOT EXISTS", and
-- D1 runs each migration exactly once, so a plain ADD COLUMN is safe here.

ALTER TABLE accounts ADD COLUMN email_bi TEXT;
CREATE INDEX IF NOT EXISTS idx_accounts_email ON accounts(email_bi);
