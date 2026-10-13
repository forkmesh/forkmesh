-- Account-based Market star ratings with required improvement reviews
-- (docs/MARKETPLACE_RATINGS.md, app/src/marketplace_reviews.py).
--
-- One current rating per (marketplace, package, account). A rating below five
-- stars carries a structured review ("what didn't work" and "how we can
-- improve"); the server refuses one without it. Every write appends to
-- marketplace_review_history, so an edited rating never loses what was said
-- before. The anonymous per-browser package_ratings table (0165) is retired
-- for writes and stays only for its historical rows.
CREATE TABLE IF NOT EXISTS marketplace_reviews (
    review_id TEXT PRIMARY KEY,
    market TEXT NOT NULL,
    package_id TEXT NOT NULL,
    reviewer_bi TEXT NOT NULL,
    reviewer_name TEXT NOT NULL,
    reviewer_display TEXT NOT NULL DEFAULT '',
    stars INTEGER NOT NULL CHECK (stars BETWEEN 1 AND 5),
    problem TEXT NOT NULL DEFAULT '',
    improvement TEXT NOT NULL DEFAULT '',
    context TEXT NOT NULL DEFAULT '',
    comment TEXT NOT NULL DEFAULT '',
    category TEXT NOT NULL DEFAULT '',
    package_version TEXT NOT NULL DEFAULT '',
    verified_use INTEGER NOT NULL DEFAULT 0,
    helpful_count INTEGER NOT NULL DEFAULT 0,
    report_count INTEGER NOT NULL DEFAULT 0,
    hidden INTEGER NOT NULL DEFAULT 0,
    hidden_by TEXT NOT NULL DEFAULT '',
    hidden_reason TEXT NOT NULL DEFAULT '',
    hidden_at INTEGER NOT NULL DEFAULT 0,
    reply_text TEXT NOT NULL DEFAULT '',
    reply_by TEXT NOT NULL DEFAULT '',
    reply_at INTEGER NOT NULL DEFAULT 0,
    addressed_version TEXT NOT NULL DEFAULT '',
    addressed_by TEXT NOT NULL DEFAULT '',
    addressed_at INTEGER NOT NULL DEFAULT 0,
    revision INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (market, package_id, reviewer_bi)
);
CREATE INDEX IF NOT EXISTS idx_marketplace_reviews_newest
    ON marketplace_reviews(market, package_id, hidden, updated_at);
CREATE INDEX IF NOT EXISTS idx_marketplace_reviews_helpful
    ON marketplace_reviews(market, package_id, hidden, helpful_count, updated_at);
CREATE INDEX IF NOT EXISTS idx_marketplace_reviews_reported
    ON marketplace_reviews(report_count, updated_at);

-- Every rating write, oldest first per review. Also the rate-limit ledger.
CREATE TABLE IF NOT EXISTS marketplace_review_history (
    review_id TEXT NOT NULL,
    revision INTEGER NOT NULL,
    reviewer_bi TEXT NOT NULL,
    stars INTEGER NOT NULL CHECK (stars BETWEEN 1 AND 5),
    problem TEXT NOT NULL DEFAULT '',
    improvement TEXT NOT NULL DEFAULT '',
    context TEXT NOT NULL DEFAULT '',
    comment TEXT NOT NULL DEFAULT '',
    category TEXT NOT NULL DEFAULT '',
    package_version TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    PRIMARY KEY (review_id, revision)
);
CREATE INDEX IF NOT EXISTS idx_marketplace_review_history_reviewer
    ON marketplace_review_history(reviewer_bi, created_at);

-- "Helpful" votes, one per account per review.
CREATE TABLE IF NOT EXISTS marketplace_review_votes (
    review_id TEXT NOT NULL,
    voter_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (review_id, voter_bi)
);
CREATE INDEX IF NOT EXISTS idx_marketplace_review_votes_voter
    ON marketplace_review_votes(voter_bi, created_at);

-- Abuse reports, one per account per review; administrators read the queue.
CREATE TABLE IF NOT EXISTS marketplace_review_reports (
    review_id TEXT NOT NULL,
    reporter_bi TEXT NOT NULL,
    reporter_name TEXT NOT NULL,
    reason TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    resolved_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (review_id, reporter_bi)
);
CREATE INDEX IF NOT EXISTS idx_marketplace_review_reports_reporter
    ON marketplace_review_reports(reporter_bi, created_at);

-- Cached per-package aggregate, recomputed from marketplace_reviews on every
-- write that can change it (hidden reviews do not count).
CREATE TABLE IF NOT EXISTS marketplace_package_rating_stats (
    market TEXT NOT NULL,
    package_id TEXT NOT NULL,
    rating_count INTEGER NOT NULL DEFAULT 0,
    rating_total INTEGER NOT NULL DEFAULT 0,
    stars_1 INTEGER NOT NULL DEFAULT 0,
    stars_2 INTEGER NOT NULL DEFAULT 0,
    stars_3 INTEGER NOT NULL DEFAULT 0,
    stars_4 INTEGER NOT NULL DEFAULT 0,
    stars_5 INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (market, package_id)
);

-- Account-scoped install/use records. A signed-in client reports that its
-- account installed or used a package; once a package has any such record
-- (or an organization install), a rating needs one from the rater too.
CREATE TABLE IF NOT EXISTS marketplace_package_uses (
    market TEXT NOT NULL,
    package_id TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT 'client',
    first_at INTEGER NOT NULL,
    last_at INTEGER NOT NULL,
    PRIMARY KEY (market, package_id, account_bi)
);
