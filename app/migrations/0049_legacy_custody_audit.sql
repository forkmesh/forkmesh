-- Audit receipt for the explicit offline migration of historical Worker-held
-- Solana wallet seeds. This table never stores a key, encrypted artifact,
-- account/repository name, wallet address, RPC credential, or transaction.
--
-- The offline tool inserts a row in the same transaction as its optimistic
-- account key-field scrub and retired-row deletion. Nothing is removed merely
-- by applying this schema: an operator must first export every key,
-- independently move any funds, reconcile every public balance at zero, and
-- explicitly confirm.

CREATE TABLE IF NOT EXISTS legacy_custody_migration_audit (
  migration_id     TEXT PRIMARY KEY,
  artifact_sha256  TEXT NOT NULL,
  audit_sha256     TEXT NOT NULL,
  record_count     INTEGER NOT NULL,
  address_count    INTEGER NOT NULL,
  confirmed_at     INTEGER NOT NULL,
  prepared_at      INTEGER NOT NULL,
  applied_at       INTEGER NOT NULL,
  status           TEXT NOT NULL CHECK (status = 'scrubbed'),
  tool_version     TEXT NOT NULL
);

-- Key-free public evidence preserved after the corresponding secret has been
-- exported and its address freshly reconciled at a finalized zero balance.
CREATE TABLE IF NOT EXISTS legacy_custody_reconciliation (
  record_id        TEXT PRIMARY KEY,
  source_table     TEXT NOT NULL,
  public_address   TEXT NOT NULL,
  network          TEXT NOT NULL,
  balance_lamports INTEGER NOT NULL CHECK (balance_lamports = 0),
  rpc_slot         INTEGER NOT NULL,
  reconciled_at    INTEGER NOT NULL,
  artifact_sha256  TEXT NOT NULL,
  audit_sha256     TEXT NOT NULL
);
