-- Keep each World deposit-sweep receipt tied to the exact amount that was
-- broadcast. Historical receipts default to zero, which the public ledger
-- presents as amount-unavailable rather than deriving a potentially false
-- value from the purchase minimum.

ALTER TABLE world_element_purchases
  ADD COLUMN sweep_lamports INTEGER NOT NULL DEFAULT 0
  CHECK (sweep_lamports >= 0);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:dd7c15b1a5a023021a02df281faaef1a1111b14d641ef194df2c59fd5c8e4167',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
