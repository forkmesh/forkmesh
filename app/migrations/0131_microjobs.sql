-- Paid microjobs: tiny, independently verified community contributions.
--
-- One row per claim. The verification result and the human-readable subject
-- live only inside the encrypted `data` envelope; the plaintext columns carry
-- exactly what the payout batcher and the rate limits need to run in SQL.
--
-- subject_bi is a blind index over "this exact piece of work" (repo + issue
-- title, repo + pull number, or the posted URL). Its UNIQUE constraint is the
-- de-duplication: the same issue, the same merged pull, or the same page can
-- be claimed once, by whoever got there first.
--
-- Nothing in this table is or references private key material. amount_lamports
-- is the fixed catalog price of the job; a payout only ever happens when the
-- repository owner's own desktop client signs the public chain_intents plan
-- that intent_id points at.
CREATE TABLE IF NOT EXISTS microjob_claims (
    claim_id TEXT PRIMARY KEY,
    account_bi TEXT NOT NULL,
    job TEXT NOT NULL
        CHECK (job IN ('issue_report','merged_pull','referral_post')),
    subject_bi TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending_review'
        CHECK (status IN
            ('pending_review','approved','queued','paid','rejected')),
    amount_lamports INTEGER NOT NULL DEFAULT 0
        CHECK (amount_lamports >= 0),
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    intent_id TEXT NOT NULL DEFAULT '',
    tx_signature TEXT NOT NULL DEFAULT ''
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_microjob_claims_subject
    ON microjob_claims(subject_bi);
CREATE INDEX IF NOT EXISTS idx_microjob_claims_account
    ON microjob_claims(account_bi, created_at);
CREATE INDEX IF NOT EXISTS idx_microjob_claims_status
    ON microjob_claims(status, created_at);
CREATE INDEX IF NOT EXISTS idx_microjob_claims_intent
    ON microjob_claims(intent_id);

-- Native migrations are authoritative for deployed databases. Append this
-- build's exact runtime DDL fingerprint only after the table exists; the
-- markers are append-only, so a still-live prior Worker stays safe through a
-- rolling cutover.
INSERT INTO schema_meta (k, v)
VALUES (
    'schema_fingerprint:0fd7c197077c08df81654f822681b8362800021e048d0832c87747c766568533',
    'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
