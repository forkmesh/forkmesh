-- Freeze the economic and human context of a contributor payment. The
-- summary is deliberately bounded and plain text; full issue/PR bodies,
-- comments, attachments, and patches remain outside the financial ledger.
ALTER TABLE contributor_reward_estimates
ADD COLUMN subject_summary TEXT NOT NULL DEFAULT ''
CHECK (length(subject_summary) <= 360);

ALTER TABLE contributor_reward_estimates
ADD COLUMN payment_usd_micros INTEGER NOT NULL DEFAULT 0
CHECK (payment_usd_micros >= 0);

ALTER TABLE contributor_reward_estimates
ADD COLUMN payment_sol_usd_micros INTEGER NOT NULL DEFAULT 0
CHECK (payment_sol_usd_micros >= 0);

ALTER TABLE contributor_reward_estimates
ADD COLUMN payment_valued_at INTEGER NOT NULL DEFAULT 0
CHECK (payment_valued_at >= 0);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:6eb9cc21210309a8a7bf5c0c2fbca0aafd73eefabbaef3378953e60169252e39',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
