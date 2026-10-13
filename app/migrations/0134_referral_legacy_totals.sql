-- Preserve the audit remainder from the retired aggregate-only referral
-- counter. These rows are informational only: ranking and rewards derive from
-- verified referral_attributions with decision='credited'.

CREATE TABLE IF NOT EXISTS referral_legacy_totals (
  referrer_bi TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  unattributed_signups INTEGER NOT NULL DEFAULT 0,
  migrated_at INTEGER NOT NULL DEFAULT 0
);

-- Filled with the exact runtime schema fingerprint by the schema contract.
INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:dd9dbd3dd68500c88662d13563c9ff0ba27aff50496519367aa4d3d1f8d101da',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
