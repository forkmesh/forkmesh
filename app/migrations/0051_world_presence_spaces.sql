-- Consented, generalized Town Square inactivity cards.
--
-- account_bi is a keyed blind index. The public card (chosen display name,
-- generalized status, server-derived account badge, and bounded mirror-belt
-- count) is encrypted in data. No IP address, exact location, URL, search,
-- form content, or behavioral history is stored. The Worker prunes expired
-- rows and a user can remove their row immediately.
CREATE TABLE IF NOT EXISTS world_inactive_presence (
    account_bi TEXT PRIMARY KEY,
    data TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_world_inactive_presence_expiry
ON world_inactive_presence(expires_at);
