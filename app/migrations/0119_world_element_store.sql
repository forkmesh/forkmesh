-- Purchases of World store elements. Public data only: a per-purchase Solana
-- Pay reference address (a random pubkey nobody holds a key for), the finalized
-- signature of the buyer's own wallet-signed transfer, and the split owed to
-- mirror nodes. ForkMesh never creates a deposit wallet and never stores a key;
-- the element itself is granted on the account record.
CREATE TABLE IF NOT EXISTS world_element_purchases (
    purchase_id TEXT PRIMARY KEY,
    account_bi TEXT NOT NULL,
    element_id TEXT NOT NULL,
    amount_lamports INTEGER NOT NULL,
    treasury_lamports INTEGER NOT NULL DEFAULT 0,
    mirror_lamports INTEGER NOT NULL DEFAULT 0,
    reference_address TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'prepared',
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    tx_signature TEXT NOT NULL DEFAULT '',
    source_address TEXT NOT NULL DEFAULT '',
    confirmed_at INTEGER NOT NULL DEFAULT 0,
    distribution_intent_id TEXT NOT NULL DEFAULT '',
    -- Custodial deposit path: a temporary per-purchase address the Worker
    -- generates and signs the sweep for. deposit_secret is an encrypted blob
    -- and is cleared as soon as the sweep confirms.
    method TEXT NOT NULL DEFAULT 'direct',
    deposit_address TEXT NOT NULL DEFAULT '',
    deposit_secret TEXT NOT NULL DEFAULT '',
    sweep_signature TEXT NOT NULL DEFAULT '',
    sweep_at INTEGER NOT NULL DEFAULT 0,
    sweep_error TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_world_element_purchases_deposit
    ON world_element_purchases(status, method, expires_at);

CREATE UNIQUE INDEX IF NOT EXISTS idx_world_element_purchase_signature
    ON world_element_purchases(tx_signature) WHERE tx_signature<>'';

CREATE INDEX IF NOT EXISTS idx_world_element_purchases_account
    ON world_element_purchases(account_bi, created_at);

CREATE INDEX IF NOT EXISTS idx_world_element_purchases_status
    ON world_element_purchases(status, created_at);
