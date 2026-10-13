-- Optional one-use signup gate shared by platform administrators. The code's
-- searchable representation is a keyed blind index; its display value is
-- encrypted with the Worker data key. Only one generation can be current.

CREATE TABLE IF NOT EXISTS admin_signup_referral_code (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  code_bi TEXT NOT NULL,
  data TEXT NOT NULL,
  generation INTEGER NOT NULL DEFAULT 1 CHECK (generation >= 1),
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  last_used_at INTEGER NOT NULL DEFAULT 0,
  last_used_by_bi TEXT NOT NULL DEFAULT ''
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_admin_signup_referral_code_bi
  ON admin_signup_referral_code(code_bi);

-- Provider lifecycle aggregates used by the administrator Users view. URLs
-- are intentionally not stored because confirmation links carry credentials.
ALTER TABLE mailtrap_email_sends
  ADD COLUMN opened_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE mailtrap_email_sends
  ADD COLUMN clicked_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE mailtrap_email_sends
  ADD COLUMN open_count INTEGER NOT NULL DEFAULT 0 CHECK (open_count >= 0);
ALTER TABLE mailtrap_email_sends
  ADD COLUMN click_count INTEGER NOT NULL DEFAULT 0 CHECK (click_count >= 0);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:1e0aa44e62a22a40a93fccc742ba8dcfdf05b98a06b459ea595e920480a43e61',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
