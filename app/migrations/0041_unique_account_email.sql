-- ForkMesh D1 migration 0041 — one email per account (database-enforced).
-- Duplicate-email signups were previously blocked only in application code
-- (_account_signup / finalize in src/entry.py). This adds the missing database
-- backstop: a UNIQUE index on the email blind index so a second account can
-- never share an email, even under a race between two concurrent signups.
--
-- Before the constraint can be created, any pre-existing duplicates must go.
-- For each email_bi we keep the FIRST record (lowest rowid — the earliest
-- inserted) and delete the rest. Keyless nodes have no email (email_bi IS NULL,
-- and defensively ''), so they are excluded from the dedup entirely and all
-- kept. Both the authoritative `accounts` table and its `users` mirror are
-- deduped and constrained so email lookups on either side stay unique.
--
-- The matching UNIQUE index for fresh databases lives in ensure_schema()
-- (src/schema.py). SQLite/D1 runs each migration exactly once, so the
-- non-idempotent DELETE + index swap below are safe here.

-- accounts: drop duplicate-email rows, keep the earliest per email.
DELETE FROM accounts
WHERE email_bi IS NOT NULL AND email_bi <> ''
  AND rowid NOT IN (
    SELECT MIN(rowid) FROM accounts
    WHERE email_bi IS NOT NULL AND email_bi <> ''
    GROUP BY email_bi
  );

-- users mirror: same dedup, independently keeping the earliest per email.
DELETE FROM users
WHERE email_bi IS NOT NULL AND email_bi <> ''
  AND rowid NOT IN (
    SELECT MIN(rowid) FROM users
    WHERE email_bi IS NOT NULL AND email_bi <> ''
    GROUP BY email_bi
  );

-- Replace the non-unique lookup index with a partial UNIQUE index. The partial
-- WHERE leaves keyless (NULL/'' email) rows unconstrained so many can coexist.
DROP INDEX IF EXISTS idx_accounts_email;
CREATE UNIQUE INDEX idx_accounts_email ON accounts(email_bi)
  WHERE email_bi IS NOT NULL AND email_bi <> '';

DROP INDEX IF EXISTS idx_users_email;
CREATE UNIQUE INDEX idx_users_email ON users(email_bi)
  WHERE email_bi IS NOT NULL AND email_bi <> '';
