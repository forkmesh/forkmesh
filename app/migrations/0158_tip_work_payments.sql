-- Give direct member payments enough immutable context to stand on their own
-- in the public ledger.  The transfer is still signed by the sender's local
-- desktop wallet; these fields contain only the public receipt and the work it
-- paid for.
ALTER TABLE tip_notice ADD COLUMN task_description TEXT NOT NULL DEFAULT ''
    CHECK (length(task_description) <= 360);

ALTER TABLE tip_notice ADD COLUMN output_url TEXT NOT NULL DEFAULT ''
    CHECK (length(output_url) <= 1000);

ALTER TABLE tip_notice ADD COLUMN entered_currency TEXT NOT NULL DEFAULT 'SOL'
    CHECK (entered_currency IN ('SOL','USD'));

ALTER TABLE tip_notice ADD COLUMN entered_usd_micros INTEGER NOT NULL DEFAULT 0
    CHECK (entered_usd_micros >= 0);

ALTER TABLE tip_notice ADD COLUMN sol_usd_price_micros INTEGER NOT NULL DEFAULT 0
    CHECK (sol_usd_price_micros >= 0);

ALTER TABLE tip_notice ADD COLUMN price_observed_at INTEGER NOT NULL DEFAULT 0
    CHECK (price_observed_at >= 0);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:e912244bb588a5e95f659709a08e3d15a50f22e954c73c504a6c5c4786288f28',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
