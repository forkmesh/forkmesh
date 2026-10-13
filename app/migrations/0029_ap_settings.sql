-- Fediverse operator configuration: admin-managed enable/disable switch and
-- the remote-domain blocklist (defederation), driven by the signed admin API
-- (/api/accounts/admin-ap, /api/accounts/admin-ap-update).
CREATE TABLE IF NOT EXISTS ap_settings (
    k TEXT PRIMARY KEY, v TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS ap_blocked_domains (
    domain TEXT PRIMARY KEY, added_by TEXT, added_at INTEGER NOT NULL);
