-- Non-custodial community reward scheduling and voluntary contribution state.
-- Private keys are neither accepted nor stored by any of these tables.

CREATE TABLE IF NOT EXISTS reward_node_observations (
    node_bi TEXT PRIMARY KEY,
    first_verified_at INTEGER NOT NULL,
    last_verified_at INTEGER NOT NULL,
    consecutive_checks INTEGER NOT NULL DEFAULT 1,
    contribution_units INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_reward_node_observations_last
    ON reward_node_observations(last_verified_at);

CREATE TABLE IF NOT EXISTS reward_rounds (
    round_id TEXT PRIMARY KEY,
    status TEXT NOT NULL DEFAULT 'scheduled',
    interval_start INTEGER NOT NULL,
    scheduled_at INTEGER NOT NULL,
    execute_after INTEGER NOT NULL,
    schedule_entropy TEXT NOT NULL,
    snapshot_data TEXT NOT NULL,
    selection_data TEXT NOT NULL DEFAULT '',
    intent_id TEXT NOT NULL DEFAULT '',
    completed_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_reward_rounds_status
    ON reward_rounds(status, execute_after);

CREATE TABLE IF NOT EXISTS reward_contributions (
    contribution_id TEXT PRIMARY KEY,
    mode TEXT NOT NULL,
    amount_lamports INTEGER NOT NULL,
    reference_address TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'prepared',
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    tx_signature TEXT NOT NULL DEFAULT '',
    source_address TEXT NOT NULL DEFAULT '',
    confirmed_at INTEGER NOT NULL DEFAULT 0,
    distribution_intent_id TEXT NOT NULL DEFAULT ''
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_reward_contribution_signature
    ON reward_contributions(tx_signature) WHERE tx_signature<>'';
CREATE INDEX IF NOT EXISTS idx_reward_contributions_status
    ON reward_contributions(status, created_at);

CREATE TABLE IF NOT EXISTS reward_contribution_intents (
    contribution_id TEXT NOT NULL,
    intent_id TEXT NOT NULL,
    chunk_index INTEGER NOT NULL,
    chunk_count INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending_signature',
    PRIMARY KEY (contribution_id, intent_id)
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_reward_contribution_intent_id
    ON reward_contribution_intents(intent_id);
