-- Reverse 0154. Existing payment signatures and SOL amounts are untouched;
-- only the additive compact context and USD valuation snapshot are removed.
ALTER TABLE contributor_reward_estimates DROP COLUMN payment_valued_at;
ALTER TABLE contributor_reward_estimates DROP COLUMN payment_sol_usd_micros;
ALTER TABLE contributor_reward_estimates DROP COLUMN payment_usd_micros;
ALTER TABLE contributor_reward_estimates DROP COLUMN subject_summary;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:6eb9cc21210309a8a7bf5c0c2fbca0aafd73eefabbaef3378953e60169252e39';
