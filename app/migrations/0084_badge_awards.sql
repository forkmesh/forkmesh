-- Achievement badges (adhoc #370): public recognition marks awarded once per
-- (badge, account), either automatically at a real platform event (first 100
-- signups, an hour in the World, the first successful referral, becoming a
-- mirror operator) or by a platform administrator. The fixed badge catalog
-- (name/description/icon per slug) lives in code (src/badges.py), not in D1.
--
-- account_bi is a blind index; name is the plaintext public username (same
-- trust level as profile_follows / referral_stats.name). granted_by is
-- 'system' for an automatic award or the granting admin's username. Awards
-- are permanent: later losing eligibility does not revoke an already-earned
-- badge.
CREATE TABLE IF NOT EXISTS badge_awards (
    badge_slug TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    name TEXT NOT NULL,
    granted_by TEXT NOT NULL DEFAULT 'system',
    created_at INTEGER NOT NULL,
    PRIMARY KEY (badge_slug, account_bi));
CREATE INDEX IF NOT EXISTS idx_badge_awards_account ON badge_awards(account_bi);
