-- Where a blog post was announced. One row per post and network holding the
-- public permalink only; the writer is retained as a blind index so an edit is
-- attributable without storing an account name. No visitor data of any kind.
CREATE TABLE IF NOT EXISTS blog_post_links (
    slug TEXT NOT NULL,
    network TEXT NOT NULL CHECK (network IN ('reddit', 'mastodon', 'twitter')),
    url TEXT NOT NULL DEFAULT '' CHECK (length(url) <= 512),
    updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0),
    updated_by_bi TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (slug, network)
) WITHOUT ROWID;

-- Native migrations are authoritative for deployed databases. Append this
-- build's exact runtime DDL fingerprint only after the table exists; the
-- markers are append-only, so a still-live prior Worker stays safe through a
-- rolling cutover.
INSERT INTO schema_meta (k, v)
VALUES (
    'schema_fingerprint:992cc21a1e6ede25c4907b5d2130ca0915bb35262a8e24c575f6c965a7e6565b',
    'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
