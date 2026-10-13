-- Anonymous, per-browser ratings for public Market entries. Re-rating a
-- package replaces the browser's existing value, so the aggregate is not an
-- event counter.
CREATE TABLE IF NOT EXISTS package_ratings (
    run TEXT NOT NULL,
    market TEXT NOT NULL DEFAULT 'forkmesh',
    pkg TEXT NOT NULL,
    rating INTEGER NOT NULL CHECK (rating BETWEEN 1 AND 5),
    ts INTEGER NOT NULL,
    PRIMARY KEY (run, market, pkg)
);
CREATE INDEX IF NOT EXISTS idx_package_ratings_ts ON package_ratings(ts);
CREATE INDEX IF NOT EXISTS idx_package_ratings_pkg
    ON package_ratings(market, pkg);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:1068886580e9d1a1b62add34e2a892e8a5d0daa5d07905b14695aae80e7be584',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
