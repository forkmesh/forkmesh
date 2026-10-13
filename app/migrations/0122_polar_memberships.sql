-- Polar checkout customer mapping and subscription-derived platform roles.
-- Provider identifiers are stored only inside DATA_KEY-encrypted blobs; lookup
-- and uniqueness use keyed blind indexes.

CREATE TABLE IF NOT EXISTS polar_customers (
    account_bi TEXT PRIMARY KEY,
    external_id_bi TEXT NOT NULL UNIQUE,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS polar_memberships (
    subscription_bi TEXT PRIMARY KEY,
    account_bi TEXT NOT NULL,
    product_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    tier TEXT NOT NULL CHECK (tier IN ('supporter', 'pro')),
    status TEXT NOT NULL,
    current_period_end INTEGER NOT NULL DEFAULT 0,
    provider_modified_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_polar_memberships_account
    ON polar_memberships(account_bi, current_period_end);

CREATE INDEX IF NOT EXISTS idx_polar_memberships_status
    ON polar_memberships(status, current_period_end);

CREATE TABLE IF NOT EXISTS polar_webhook_events (
    event_id TEXT PRIMARY KEY,
    event_type TEXT NOT NULL,
    received_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_polar_webhook_events_received
    ON polar_webhook_events(received_at);
