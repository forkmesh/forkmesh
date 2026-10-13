-- Fail-closed legacy-wallet readiness and attested federated rewards.
--
-- Historical federated_presence rows contained only a relay-asserted wallet,
-- name and timestamp. They cannot establish mirror health, integrity, or even
-- that the node mirrors forkmesh/forkmesh, so none survive this upgrade.
DELETE FROM federated_presence;

ALTER TABLE federated_presence
  ADD COLUMN operator_id TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN device_id TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN public_key TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN base_url TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN registration_sig TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN registration_issued_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE federated_presence
  ADD COLUMN health_message TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN health_sig TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN checked_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE federated_presence
  ADD COLUMN healthy INTEGER NOT NULL DEFAULT 0;
ALTER TABLE federated_presence
  ADD COLUMN integrity TEXT NOT NULL DEFAULT 'unknown';
ALTER TABLE federated_presence
  ADD COLUMN forkmesh_active INTEGER NOT NULL DEFAULT 0;
ALTER TABLE federated_presence
  ADD COLUMN forkmesh_verified_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE federated_presence
  ADD COLUMN forkmesh_refs_sha256 TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN forkmesh_operations_sha256 TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN abuse_blocked INTEGER NOT NULL DEFAULT 0;
ALTER TABLE federated_presence
  ADD COLUMN attestation_id TEXT NOT NULL DEFAULT '';
ALTER TABLE federated_presence
  ADD COLUMN first_verified_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE federated_presence
  ADD COLUMN last_verified_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE federated_presence
  ADD COLUMN consecutive_checks INTEGER NOT NULL DEFAULT 0;

-- The originating relay now retains the exact node-signed health message, not
-- merely its signature, so the approved main relay can independently verify
-- the repository-health attestation.
ALTER TABLE mirror_https_endpoints
  ADD COLUMN health_message TEXT NOT NULL DEFAULT '';

-- Force the upgraded Worker to inspect every historical key-bearing shape
-- before its D1-backed API can report ready. The explicit offline scrub writes
-- this marker only after all optimistic row updates and its receipt succeed.
CREATE TABLE IF NOT EXISTS schema_meta (
  k TEXT PRIMARY KEY,
  v TEXT NOT NULL
);
DELETE FROM schema_meta WHERE k = 'legacy_wallet_custody_v2';

CREATE TABLE IF NOT EXISTS legacy_custody_reconciliation (
  record_id TEXT PRIMARY KEY,
  source_table TEXT NOT NULL,
  public_address TEXT NOT NULL,
  network TEXT NOT NULL,
  balance_lamports INTEGER NOT NULL CHECK (balance_lamports = 0),
  rpc_slot INTEGER NOT NULL,
  reconciled_at INTEGER NOT NULL,
  artifact_sha256 TEXT NOT NULL,
  audit_sha256 TEXT NOT NULL
);
