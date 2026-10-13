-- Persist a bounded read-only scan of historical System transfers involving
-- public addresses ForkMesh can prove belong to the system. Canonical D1
-- transaction rows remain authoritative and are de-duplicated at read time.
-- Arbitrary member wallet histories are never enrolled.

CREATE TABLE IF NOT EXISTS ledger_backfill_sources (
  source_id TEXT PRIMARY KEY,
  source_kind TEXT NOT NULL,
  address TEXT NOT NULL,
  network TEXT NOT NULL,
  subject_repo_bi TEXT NOT NULL DEFAULT '',
  subject_kind TEXT NOT NULL DEFAULT '',
  subject_number INTEGER NOT NULL DEFAULT 0,
  before_signature TEXT NOT NULL DEFAULT '',
  completed INTEGER NOT NULL DEFAULT 0 CHECK (completed IN (0, 1)),
  updated_at INTEGER NOT NULL DEFAULT 0,
  UNIQUE (network, address)
);

CREATE INDEX IF NOT EXISTS idx_ledger_backfill_sources_pending
  ON ledger_backfill_sources(completed, updated_at, source_id);

CREATE TABLE IF NOT EXISTS ledger_backfill_transactions (
  entry_id TEXT PRIMARY KEY,
  network TEXT NOT NULL,
  signature TEXT NOT NULL,
  instruction_index INTEGER NOT NULL,
  block_time INTEGER NOT NULL DEFAULT 0,
  source_address TEXT NOT NULL,
  destination_address TEXT NOT NULL,
  amount_lamports INTEGER NOT NULL CHECK (amount_lamports > 0),
  observed_address TEXT NOT NULL,
  discovered_from TEXT NOT NULL,
  UNIQUE (network, signature, instruction_index)
);

CREATE INDEX IF NOT EXISTS idx_ledger_backfill_transactions_time
  ON ledger_backfill_transactions(block_time DESC, entry_id DESC);

CREATE INDEX IF NOT EXISTS idx_ledger_backfill_transactions_signature
  ON ledger_backfill_transactions(signature);

-- Keep canonical-signature suppression indexed as historical coverage grows.
CREATE INDEX IF NOT EXISTS idx_pending_rewards_tx_signature
  ON pending_rewards(tx_signature) WHERE tx_signature<>'';

CREATE INDEX IF NOT EXISTS idx_world_element_purchase_sweep_signature
  ON world_element_purchases(sweep_signature) WHERE sweep_signature<>'';

CREATE INDEX IF NOT EXISTS idx_microjob_claims_tx_signature
  ON microjob_claims(tx_signature) WHERE tx_signature<>'';

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:ac8a0509f3a7c776e64d7736174c6ce590120746a3c6b987ae5e2f99f7e48d78',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
