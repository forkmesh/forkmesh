ALTER TABLE badge_awards
       ADD COLUMN tier INTEGER NOT NULL DEFAULT 0
       CHECK (tier BETWEEN 0 AND 3);

ALTER TABLE badge_awards
       ADD COLUMN tier_at INTEGER NOT NULL DEFAULT 0
       CHECK (tier_at >= 0);

CREATE TABLE IF NOT EXISTS badge_progress (
    badge_slug TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    value INTEGER NOT NULL DEFAULT 0 CHECK (value >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    PRIMARY KEY (badge_slug, account_bi));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:e05a69371b70e57df8b6065a8af3c9bb8df4ebcbab1d573dbd55392cbfefadcb',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
