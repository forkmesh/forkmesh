-- Retain a bounded, admin-only integrity ledger for referral-link clicks.
-- A keyed source/referrer/day digest enforces uniqueness. Raw IP and
-- User-Agent evidence is stored only inside the encrypted data envelope;
-- duplicate and rejected attempts never increase the public click counter.

CREATE TABLE IF NOT EXISTS referral_click_audit (
  visit_bi      TEXT PRIMARY KEY,
  referrer_bi   TEXT NOT NULL,
  ip_bi         TEXT NOT NULL DEFAULT '',
  user_agent_bi TEXT NOT NULL DEFAULT '',
  decision      TEXT NOT NULL
    CHECK (decision IN ('accepted','duplicate','rejected')),
  risk_flags    TEXT NOT NULL DEFAULT '[]',
  data          TEXT NOT NULL,
  first_seen_at INTEGER NOT NULL,
  last_seen_at  INTEGER NOT NULL,
  repeat_count  INTEGER NOT NULL DEFAULT 1 CHECK (repeat_count > 0)
);

CREATE INDEX IF NOT EXISTS idx_referral_click_audit_referrer
  ON referral_click_audit(referrer_bi, last_seen_at DESC);
CREATE INDEX IF NOT EXISTS idx_referral_click_audit_expiry
  ON referral_click_audit(last_seen_at);

-- Filled with the exact runtime schema fingerprint by the schema contract.
INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:5bef1c598f7658581233537e5a312862886a62ad0c847f2f9f4ddfa4132731e1',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
