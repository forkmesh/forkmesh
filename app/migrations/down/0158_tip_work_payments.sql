-- Reverse 0158. The original public transfer receipt remains intact; only
-- the additive work context and entry-time USD valuation are removed.
ALTER TABLE tip_notice DROP COLUMN price_observed_at;
ALTER TABLE tip_notice DROP COLUMN sol_usd_price_micros;
ALTER TABLE tip_notice DROP COLUMN entered_usd_micros;
ALTER TABLE tip_notice DROP COLUMN entered_currency;
ALTER TABLE tip_notice DROP COLUMN output_url;
ALTER TABLE tip_notice DROP COLUMN task_description;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:e912244bb588a5e95f659709a08e3d15a50f22e954c73c504a6c5c4786288f28';
