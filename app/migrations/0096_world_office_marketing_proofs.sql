-- Private proof-of-work links for the Marketing floor.
--
-- Only opaque organization/account indexes and bounded timestamps are plain.
-- The member name, public HTTPS URL, label, and hostname stay in the sealed
-- payload and are returned only after a current Marketing-team membership
-- check.
CREATE TABLE IF NOT EXISTS world_office_marketing_proofs (
    proof_id TEXT PRIMARY KEY CHECK (
        length(proof_id) = 32
        AND proof_id NOT GLOB '*[^0-9a-f]*'
    ),
    org_bi TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    url_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at > 0)
);

CREATE INDEX IF NOT EXISTS idx_world_office_marketing_proofs_org
ON world_office_marketing_proofs(org_bi, created_at DESC, proof_id DESC);

CREATE INDEX IF NOT EXISTS idx_world_office_marketing_proofs_member
ON world_office_marketing_proofs(
    org_bi, account_bi, created_at DESC, proof_id DESC
);

CREATE TRIGGER IF NOT EXISTS trg_world_office_marketing_proofs_capacity
BEFORE INSERT ON world_office_marketing_proofs
WHEN (SELECT COUNT(*) FROM world_office_marketing_proofs) >= 10000
BEGIN
    SELECT RAISE(ABORT, 'world_office_marketing_proof_catalog_full');
END;
