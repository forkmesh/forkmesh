-- Polar is gone: the card-checkout integration it served was removed, and
-- memberships are granted through Patreon (platform-scope role_grants) and
-- the promotion scope. Nothing reads or writes these tables any more.
--
-- Dropping them deletes the customer mapping and subscription mirror. Polar
-- remains the record of any subscription that ever existed; the entitlements
-- themselves live in role_grants and are untouched here.

DROP INDEX IF EXISTS idx_polar_webhook_events_received;
DROP TABLE IF EXISTS polar_webhook_events;
DROP INDEX IF EXISTS idx_polar_memberships_status;
DROP INDEX IF EXISTS idx_polar_memberships_account;
DROP TABLE IF EXISTS polar_memberships;
DROP TABLE IF EXISTS polar_customers;

-- The runtime schema text shrank by those tables, so stamp the new
-- fingerprint:
INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:8e64f3f9a5718c0346f91cc751c914c4462b8a74f3065faef39c8a197a44de9c',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
