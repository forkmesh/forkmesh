-- Consented public links submitted at the World lobby kiosk. Human-readable
-- URLs/accounts are encrypted in data; account/url indexes are opaque. Scores
-- and traffic ranges are public kiosk output and intentionally bounded.
CREATE TABLE IF NOT EXISTS world_lobby_links (
    link_id TEXT PRIMARY KEY,
    account_bi TEXT NOT NULL,
    url_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    score INTEGER NOT NULL CHECK (score BETWEEN 0 AND 100),
    potential_low INTEGER NOT NULL CHECK (potential_low >= 0),
    potential_high INTEGER NOT NULL CHECK (potential_high >= potential_low),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE(account_bi, url_bi));
CREATE INDEX IF NOT EXISTS idx_world_lobby_links_recent
ON world_lobby_links(created_at DESC, link_id DESC);
CREATE INDEX IF NOT EXISTS idx_world_lobby_links_account
ON world_lobby_links(account_bi, created_at DESC);
