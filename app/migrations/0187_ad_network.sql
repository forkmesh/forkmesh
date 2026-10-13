-- ForkMesh Ads (app/src/ad_network.py): the first-party ad network. Any account
-- may write a short text ad; it runs only after a platform administrator
-- approves the exact creative (approved_hash = content_hash), and only while
-- the owner's membership level funds a live slot. Serving is contextual by
-- product surface, never by viewer, and the only measurement is an aggregate
-- daily click count.

CREATE TABLE IF NOT EXISTS ad_network_ads (
    ad_id TEXT PRIMARY KEY,
    account_bi TEXT NOT NULL,
    owner_sealed TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'draft' CHECK (status IN
        ('draft','pending','approved','rejected','suspended',
         'withdrawn')),
    paused INTEGER NOT NULL DEFAULT 0,
    headline TEXT NOT NULL,
    body TEXT NOT NULL DEFAULT '',
    cta TEXT NOT NULL DEFAULT '',
    sponsor TEXT NOT NULL,
    url TEXT NOT NULL,
    category TEXT NOT NULL,
    placements TEXT NOT NULL DEFAULT '',
    content_hash TEXT NOT NULL,
    approved_hash TEXT NOT NULL DEFAULT '',
    screen_flags TEXT NOT NULL DEFAULT '',
    owner_note TEXT NOT NULL DEFAULT '',
    review_reason TEXT NOT NULL DEFAULT '',
    review_codes TEXT NOT NULL DEFAULT '',
    starts_at INTEGER NOT NULL DEFAULT 0,
    ends_at INTEGER NOT NULL DEFAULT 0,
    submitted_at INTEGER NOT NULL DEFAULT 0,
    reviewed_at INTEGER NOT NULL DEFAULT 0,
    approved_until INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL);

CREATE INDEX IF NOT EXISTS idx_ad_network_ads_account ON ad_network_ads(account_bi, updated_at);

CREATE INDEX IF NOT EXISTS idx_ad_network_ads_status ON ad_network_ads(status, submitted_at);

CREATE TABLE IF NOT EXISTS ad_network_reviews (
    review_id TEXT PRIMARY KEY,
    ad_id TEXT NOT NULL,
    action TEXT NOT NULL CHECK (action IN
        ('approve','reject','suspend','reinstate','auto-suspend')),
    reviewer_bi TEXT NOT NULL DEFAULT '',
    self_review INTEGER NOT NULL DEFAULT 0,
    reason TEXT NOT NULL DEFAULT '',
    codes TEXT NOT NULL DEFAULT '',
    content_hash TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL);

CREATE INDEX IF NOT EXISTS idx_ad_network_reviews_ad ON ad_network_reviews(ad_id, created_at);

CREATE TABLE IF NOT EXISTS ad_network_reports (
    ad_id TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    reason TEXT NOT NULL,
    note TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    resolved_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (ad_id, account_bi));

CREATE INDEX IF NOT EXISTS idx_ad_network_reports_account ON ad_network_reports(account_bi);

CREATE TABLE IF NOT EXISTS ad_network_daily_stats (
    ad_id TEXT NOT NULL,
    day INTEGER NOT NULL,
    placement TEXT NOT NULL,
    clicks INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (ad_id, day, placement));

-- The runtime schema text grew (ad_network_*), so stamp the new fingerprint:
INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:c319a0e3054a58adc4bfd2bf492af3257396e6b759b5cda93c0252cae0fd65ea',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
