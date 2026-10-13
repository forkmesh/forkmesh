-- Invitation eligibility must not be asserted by a boolean in the send
-- request.  Each send references a durable, independently matched provenance
-- row.  Plaintext columns contain only keyed blind indexes; descriptive audit
-- context remains in the Worker's encrypted ``data`` blob.

ALTER TABLE contributor_invitations
    ADD COLUMN contributor_bi TEXT NOT NULL DEFAULT '';
ALTER TABLE contributor_invitations
    ADD COLUMN provenance_id TEXT NOT NULL DEFAULT '';

CREATE TABLE IF NOT EXISTS contributor_invitation_provenance (
    id TEXT PRIMARY KEY,
    repo_id TEXT NOT NULL,
    contributor_bi TEXT NOT NULL,
    email_bi TEXT NOT NULL,
    basis TEXT NOT NULL,
    created_by_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    revoked_at INTEGER NOT NULL DEFAULT 0,
    CHECK (basis IN (
        'public_for_invitations','prior_consent','owner_supplied'))
);

CREATE INDEX IF NOT EXISTS idx_contributor_invitation_provenance_match
    ON contributor_invitation_provenance(
        repo_id, contributor_bi, email_bi, revoked_at);

-- This is the concurrency boundary. A provenance row revoked after the
-- application's preflight read but before INSERT cannot authorize a send.
CREATE TRIGGER IF NOT EXISTS trg_invitation_provenance_reservation
BEFORE INSERT ON contributor_invitations
WHEN NEW.status='pending'
 AND NOT EXISTS (
     SELECT 1 FROM contributor_invitation_provenance
     WHERE id=NEW.provenance_id
       AND repo_id=NEW.repo_id
       AND contributor_bi=NEW.contributor_bi
       AND email_bi=NEW.email_bi
       AND revoked_at=0
 )
BEGIN
    SELECT RAISE(ABORT, 'invitation_provenance_invalid');
END;
