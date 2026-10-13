-- Move the optional one-use signup gate from deploy-time configuration to an
-- administrator-controlled runtime setting. Existing relays upgrade with the
-- requirement disabled; the admin console can enable it without a redeploy.

ALTER TABLE admin_signup_referral_code
  ADD COLUMN enabled INTEGER NOT NULL DEFAULT 0
  CHECK (enabled IN (0, 1));

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:fd1fdf75922662ff06c3a2e93afd4e50d01f09f2c2d2603b4fdf7a2ecf5cd12b',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
