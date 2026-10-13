-- Verified referral attribution and automated anti-gaming signals.
--
-- Raw source addresses and User-Agent strings live only in the encrypted data
-- envelope; keyed blind indexes support equality checks without searchable
-- raw values. The envelope also carries coarse country/client-category
-- evidence. A signup is not
-- credited until email verification, and rejected decisions remain auditable
-- without affecting the public referral counter.

CREATE TABLE IF NOT EXISTS referral_attributions (
  referred_bi   TEXT PRIMARY KEY,
  referred_name TEXT NOT NULL,
  referrer_bi   TEXT NOT NULL,
  referrer_name TEXT NOT NULL,
  signup_ip_bi  TEXT NOT NULL DEFAULT '',
  user_agent_bi TEXT NOT NULL DEFAULT '',
  network_window_bi TEXT NOT NULL DEFAULT '',
  decision      TEXT NOT NULL DEFAULT 'pending'
    CHECK (decision IN ('pending','credited','rejected')),
  risk_flags    TEXT NOT NULL DEFAULT '[]',
  data          TEXT NOT NULL,
  created_at    INTEGER NOT NULL,
  verified_at   INTEGER NOT NULL DEFAULT 0,
  credited_at   INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_referral_attributions_referrer
  ON referral_attributions(referrer_bi, decision, credited_at DESC);
CREATE INDEX IF NOT EXISTS idx_referral_attributions_network
  ON referral_attributions(signup_ip_bi, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_referral_attributions_agent
  ON referral_attributions(user_agent_bi, created_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_referral_attributions_credit_slot
  ON referral_attributions(network_window_bi)
  WHERE decision = 'credited' AND network_window_bi <> '';

-- One blind-indexed source/referrer/day key prevents refresh loops from
-- manufacturing click totals. Rows are pruned by the click writer.
CREATE TABLE IF NOT EXISTS referral_click_seen (
  visit_bi    TEXT PRIMARY KEY,
  referrer_bi TEXT NOT NULL,
  seen_at     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_referral_click_seen_expiry
  ON referral_click_seen(seen_at);

-- The exact runtime schema fingerprint is filled from the composed lazy DDL.
INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:396567955a5612326edf04bf0e0dec8a8be94106b586fd1b04668a58c9a1be57',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
