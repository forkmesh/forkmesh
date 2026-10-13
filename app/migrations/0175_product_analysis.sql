-- Internal product dashboard (/product-analysis): signup origin and per-day
-- clone counts.
--
-- signup_origin records the acquisition channel of an account so the dashboard
-- can separate accounts that came through the job-application flow and the
-- referral programme from organic signups. It is a plain category, never a
-- secret, so it lives beside is_admin as an operational column. 'unknown' is
-- the default and the honest answer for every account the derivation rules in
-- www/docs/admin-dashboard.md cannot classify.
--
-- The referral half of the backfill is expressible in SQL: an account with a
-- referral_attributions row arrived through a referral link, whatever the
-- later credit decision was. The organic half depends on the referral marker
-- inside the encrypted account record, which SQL cannot read; the Worker
-- performs that pass once, guarded by a schema_meta marker, the first time an
-- authorised founder opens the dashboard.
--
-- product_clone_daily buckets every upload-pack served through the public
-- HTTPS mirror route by UTC day. mirror_serve_counters only holds cumulative
-- totals, so "clones in the last 30 days" needs its own row per day.
ALTER TABLE users ADD COLUMN signup_origin TEXT NOT NULL DEFAULT 'unknown'
    CHECK (signup_origin IN ('job_flow','referral','organic','unknown'));

UPDATE users SET signup_origin = 'referral'
 WHERE signup_origin = 'unknown'
   AND user_bi IN (SELECT referred_bi FROM referral_attributions);

CREATE TABLE IF NOT EXISTS product_clone_daily (
    day TEXT NOT NULL,
    owner TEXT NOT NULL,
    repo TEXT NOT NULL,
    clones INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (day, owner, repo));

CREATE INDEX IF NOT EXISTS idx_product_clone_daily_day
    ON product_clone_daily(day);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:2c1fae86d4f41ca77a21db58b3ab14cee06447b33fe25a41c259043be995944d',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
