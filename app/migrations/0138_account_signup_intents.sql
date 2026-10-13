-- Password-based account creation must originate from the canonical /signup
-- page. The Worker issues a signed HttpOnly cookie backed by one row here and
-- consumes it atomically before inserting the user, preventing direct API
-- signup and replay. Claims retain no raw browser or account data.

CREATE TABLE IF NOT EXISTS account_signup_intents (
  claim_id TEXT PRIMARY KEY,
  issued_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  email_field_name TEXT NOT NULL,
  consumed_at INTEGER NOT NULL DEFAULT 0,
  account_bi TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_account_signup_intents_expiry
  ON account_signup_intents(expires_at, consumed_at);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:3527112891dd5153321f908102d4099aa047af5d6ffc4900f558662e2b24ea27',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
