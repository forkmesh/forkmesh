CREATE TABLE IF NOT EXISTS marketplace_package_activity (
    market TEXT NOT NULL,
    package_id TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    account_name TEXT NOT NULL DEFAULT '',
    views INTEGER NOT NULL DEFAULT 0,
    opens INTEGER NOT NULL DEFAULT 0,
    first_at INTEGER NOT NULL,
    last_at INTEGER NOT NULL,
    PRIMARY KEY (market, package_id, account_bi)
);
CREATE INDEX IF NOT EXISTS idx_marketplace_package_activity_package
    ON marketplace_package_activity(market, package_id, opens, views);
