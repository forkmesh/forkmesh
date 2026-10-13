-- Founders outreach moved into the CRM package (adhoc #2145).
-- The site-wide /outreach console and its /api/outreach routes are gone; the
-- CRM package now owns founders outreach per organization behind an
-- owner/admin-managed roster (crm_outreach_team) with its own sealed send log
-- (crm_outreach_log), created by the package's install migration. The core
-- roster and send log they replace have no reader left, so they are dropped.
-- users.enable_outreach stays: it is part of the user row layout copied by
-- the quarantine tables and shown in the admin users directory.
DROP INDEX IF EXISTS idx_outreach_log_sender_ts;
DROP TABLE IF EXISTS outreach_log;
DROP TABLE IF EXISTS outreach_team;

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:c05700d9ee07fbd9beccc8b73d964f4843859fb9731220465fa2b2fd45faf949',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
