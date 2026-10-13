-- ForkMesh D1 migration 0012 — relay federation (historical schema).
-- The signup-custody behavior that originally used federated_signup is disabled.
-- Existing encrypted rows are frozen for the explicit offline custody migration;
-- only public historical status remains readable. Relay identity and public
-- presence federation remain active.
--
-- A relay is identified by its Ed25519 pubkey and must be approved before it can
-- submit node attestations for independent reward verification:
--   UPDATE relays SET status = 'approved' WHERE label = '<relay label>';
-- Block a relay:
--   UPDATE relays SET status = 'blocked'  WHERE pubkey = '<relay pubkey>';

CREATE TABLE IF NOT EXISTS relays (
    relay_bi TEXT PRIMARY KEY,
    pubkey TEXT NOT NULL,
    label TEXT,
    base_url TEXT,
    status TEXT NOT NULL DEFAULT 'pending',
    registered_at INTEGER,
    approved_at INTEGER
);

-- Historical base columns only. Migration 0057 removes wallet-only rows and
-- adds node/relay-signed health, integrity and ForkMesh-mirroring evidence
-- before any federated row can participate in rewards.
CREATE TABLE IF NOT EXISTS federated_presence (
    relay_bi TEXT NOT NULL,
    wallet TEXT NOT NULL,
    name TEXT,
    ts INTEGER NOT NULL,
    PRIMARY KEY (relay_bi, wallet)
);
CREATE INDEX IF NOT EXISTS idx_federated_presence_ts ON federated_presence(ts);

-- Historical signup rows may contain donation_secret until explicitly migrated.
-- The live Worker never creates, sweeps, or rewrites that wallet material.
CREATE TABLE IF NOT EXISTS federated_signup (
    reference TEXT PRIMARY KEY,
    relay_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_federated_signup_relay ON federated_signup(relay_bi);
