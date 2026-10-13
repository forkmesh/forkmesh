-- Per-post read dedupe: a reload used to count as another view, so a post's
-- "views" number was a page-load counter rather than a reach number. One row
-- per post per blinded reader records that this reader was already counted,
-- and which source they last arrived from, so neither the total nor the
-- referrer board double-counts a refresh. `viewer_key` is the same per-slug
-- one-way digest the unique-count projection uses: no address, user agent, or
-- session is stored, and rows age out on the hourly referrer prune.
-- `counted_at` is the last read that moved the counter, kept apart from
-- `last_at`: measuring the window from the latest hit would let a reader who
-- reloads every few hours never count a second time.
CREATE TABLE IF NOT EXISTS blog_post_view_seen (
    slug TEXT NOT NULL,
    viewer_key TEXT NOT NULL,
    first_at INTEGER NOT NULL CHECK (first_at >= 0),
    last_at INTEGER NOT NULL CHECK (last_at >= 0),
    counted_at INTEGER NOT NULL DEFAULT 0 CHECK (counted_at >= 0),
    hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1),
    referrer_host TEXT NOT NULL DEFAULT '' CHECK (length(referrer_host) <= 100),
    PRIMARY KEY (slug, viewer_key)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_blog_post_view_seen_age
    ON blog_post_view_seen(last_at);

-- Native migrations are authoritative for deployed databases. Append this
-- build's exact runtime DDL fingerprint only after the table exists; the
-- markers are append-only, so a still-live prior Worker stays safe through a
-- rolling cutover.
INSERT INTO schema_meta (k, v)
VALUES (
    'schema_fingerprint:fb65fe7e8d8f87730bbd5af42bebf74f06f1bc19ed846b1b413518177e8768d4',
    'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
