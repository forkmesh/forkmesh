-- Owner-authored issue and pull-request reward estimates. These rows are
-- administrative payment records, never user claims or Worker-held funds.
CREATE TABLE IF NOT EXISTS contributor_reward_estimates (
    reward_id TEXT PRIMARY KEY,
    recipient_bi TEXT NOT NULL,
    recipient_name TEXT NOT NULL,
    repo_owner TEXT NOT NULL,
    repo_name TEXT NOT NULL,
    source_kind TEXT NOT NULL CHECK (source_kind IN ('issue','pull')),
    subject_number INTEGER NOT NULL CHECK (subject_number > 0),
    subject_title TEXT NOT NULL DEFAULT '',
    amount_lamports INTEGER NOT NULL
        CHECK (amount_lamports BETWEEN 1000000 AND 5000000),
    condition TEXT NOT NULL CHECK (condition IN ('accepted_report','merged')),
    status TEXT NOT NULL
        CHECK (status IN ('pending_merge','pending_wallet','ready',
                          'submitted','paid','cancelled')),
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    submitted_at INTEGER NOT NULL DEFAULT 0,
    paid_at INTEGER NOT NULL DEFAULT 0,
    source_address TEXT NOT NULL DEFAULT '',
    destination_address TEXT NOT NULL DEFAULT '',
    tx_signature TEXT NOT NULL DEFAULT ''
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_contributor_reward_subject
    ON contributor_reward_estimates(
        repo_owner, repo_name, source_kind, subject_number);
CREATE INDEX IF NOT EXISTS idx_contributor_reward_status
    ON contributor_reward_estimates(status, created_at);
CREATE INDEX IF NOT EXISTS idx_contributor_reward_recipient
    ON contributor_reward_estimates(recipient_bi, status);
CREATE UNIQUE INDEX IF NOT EXISTS idx_contributor_reward_tx
    ON contributor_reward_estimates(tx_signature) WHERE tx_signature<>'';

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:26795ae927ea04edc3b7d5a2fd8155841cb6971689a408ba8741fdab65c6a004',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
