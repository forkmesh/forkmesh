-- Keep the relay's secure-fetch signing material outside the public local
-- actor inventory. The encrypted key blob can move directly without exposing
-- its private component.
CREATE TABLE IF NOT EXISTS ap_service_keys (
    key_name TEXT PRIMARY KEY,
    pubkey_pem TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL
);

INSERT INTO ap_service_keys (key_name, pubkey_pem, data, created_at)
SELECT 'instance', pubkey_pem, data, created_at
FROM ap_actors
WHERE kind = 'instance'
ON CONFLICT(key_name) DO NOTHING;

-- Do not delete legacy actor, follower, or object rows here. A migration is
-- permanent and cannot prove that every encrypted actor record has already
-- been reclassified correctly. Public routing decides which actors are
-- currently exposed; retained social-graph rows remain available if an actor
-- is re-enabled or a classification bug is fixed.
