-- Completed pull requests can carry a larger owner-reviewed reward than a
-- bug report. Rebuild the table because SQLite cannot alter a CHECK constraint
-- in place; every financial and receipt column is copied verbatim.
CREATE TABLE contributor_reward_estimates_wider_pull_rewards (
    reward_id TEXT PRIMARY KEY,
    recipient_bi TEXT NOT NULL,
    recipient_name TEXT NOT NULL,
    repo_owner TEXT NOT NULL,
    repo_name TEXT NOT NULL,
    source_kind TEXT NOT NULL CHECK (source_kind IN ('issue','pull')),
    subject_number INTEGER NOT NULL CHECK (subject_number > 0),
    subject_title TEXT NOT NULL DEFAULT '',
    subject_summary TEXT NOT NULL DEFAULT ''
        CHECK (length(subject_summary) <= 360),
    amount_lamports INTEGER NOT NULL
        CHECK ((source_kind='issue' AND
                amount_lamports BETWEEN 1000000 AND 5000000) OR
               (source_kind='pull' AND
                amount_lamports BETWEEN 1000000 AND 25000000)),
    condition TEXT NOT NULL CHECK (condition IN ('accepted_report','merged')),
    status TEXT NOT NULL
        CHECK (status IN ('pending_merge','pending_wallet','ready',
                          'submitted','paid','cancelled')),
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    submitted_at INTEGER NOT NULL DEFAULT 0,
    paid_at INTEGER NOT NULL DEFAULT 0,
    payment_usd_micros INTEGER NOT NULL DEFAULT 0
        CHECK (payment_usd_micros >= 0),
    payment_sol_usd_micros INTEGER NOT NULL DEFAULT 0
        CHECK (payment_sol_usd_micros >= 0),
    payment_valued_at INTEGER NOT NULL DEFAULT 0
        CHECK (payment_valued_at >= 0),
    source_address TEXT NOT NULL DEFAULT '',
    destination_address TEXT NOT NULL DEFAULT '',
    tx_signature TEXT NOT NULL DEFAULT ''
);

INSERT INTO contributor_reward_estimates_wider_pull_rewards (
    reward_id, recipient_bi, recipient_name, repo_owner, repo_name,
    source_kind, subject_number, subject_title, subject_summary,
    amount_lamports, condition, status, created_at, updated_at, submitted_at,
    paid_at, payment_usd_micros, payment_sol_usd_micros, payment_valued_at,
    source_address, destination_address, tx_signature
)
SELECT
    reward_id, recipient_bi, recipient_name, repo_owner, repo_name,
    source_kind, subject_number, subject_title, subject_summary,
    amount_lamports, condition, status, created_at, updated_at, submitted_at,
    paid_at, payment_usd_micros, payment_sol_usd_micros, payment_valued_at,
    source_address, destination_address, tx_signature
FROM contributor_reward_estimates;

DROP TABLE contributor_reward_estimates;
ALTER TABLE contributor_reward_estimates_wider_pull_rewards
    RENAME TO contributor_reward_estimates;

CREATE UNIQUE INDEX idx_contributor_reward_subject
    ON contributor_reward_estimates(
        repo_owner, repo_name, source_kind, subject_number);
CREATE INDEX idx_contributor_reward_status
    ON contributor_reward_estimates(status, created_at);
CREATE INDEX idx_contributor_reward_recipient
    ON contributor_reward_estimates(recipient_bi, status);
CREATE UNIQUE INDEX idx_contributor_reward_tx
    ON contributor_reward_estimates(tx_signature) WHERE tx_signature<>'';

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:6b4801fb3f51ca5fa23c1492de0b72bd465391974374263779ad3d9013f55b16',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
