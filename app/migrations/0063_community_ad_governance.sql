-- Community-reviewed contextual placements. This ledger is deliberately
-- separate from wallets, pending rewards, and the community reward pool.
CREATE TABLE IF NOT EXISTS community_ad_instance_policy (
    instance_id TEXT PRIMARY KEY,
    enabled INTEGER NOT NULL DEFAULT 0 CHECK (enabled IN (0, 1)),
    contexts TEXT NOT NULL DEFAULT '[]',
    revenue_destination TEXT NOT NULL DEFAULT '',
    revenue_destination_type TEXT NOT NULL DEFAULT ''
        CHECK (revenue_destination_type IN (
            '', 'project-operations', 'instance-operations',
            'community-grants', 'nonprofit'
        )),
    updated_by_bi TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS community_ad_proposals (
    proposal_id TEXT PRIMARY KEY,
    proposer_bi TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN (
        'voting', 'approved', 'rejected', 'expired',
        'suspended', 'appealed'
    )),
    data TEXT NOT NULL,
    opens_at INTEGER NOT NULL,
    closes_at INTEGER NOT NULL,
    review_due_at INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_community_ad_proposals_status_review
    ON community_ad_proposals(status, review_due_at);

CREATE TABLE IF NOT EXISTS community_ad_votes (
    proposal_id TEXT NOT NULL,
    voter_bi TEXT NOT NULL,
    choice TEXT NOT NULL CHECK (choice IN ('approve', 'reject', 'abstain')),
    conflict INTEGER NOT NULL DEFAULT 0 CHECK (conflict IN (0, 1)),
    disclosure TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (proposal_id, voter_bi)
);
CREATE INDEX IF NOT EXISTS idx_community_ad_votes_proposal
    ON community_ad_votes(proposal_id, choice);

CREATE TABLE IF NOT EXISTS community_ad_moderation (
    record_id TEXT PRIMARY KEY,
    proposal_id TEXT NOT NULL,
    action TEXT NOT NULL CHECK (action IN (
        'note', 'suspend', 'reinstate', 'reject', 'approve',
        'appeal-filed', 'appeal-upheld', 'appeal-denied'
    )),
    reason TEXT NOT NULL,
    evidence_url TEXT NOT NULL DEFAULT '',
    actor_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_community_ad_moderation_proposal
    ON community_ad_moderation(proposal_id, created_at);

CREATE TABLE IF NOT EXISTS community_ad_revenue (
    entry_id TEXT PRIMARY KEY,
    instance_id TEXT NOT NULL,
    proposal_id TEXT NOT NULL,
    ledger_class TEXT NOT NULL DEFAULT 'advertising-revenue'
        CHECK (ledger_class = 'advertising-revenue'),
    amount_minor INTEGER NOT NULL CHECK (amount_minor >= 0),
    currency TEXT NOT NULL,
    destination_snapshot TEXT NOT NULL,
    external_reference TEXT NOT NULL,
    recorded_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_community_ad_revenue_instance
    ON community_ad_revenue(instance_id, recorded_at);

CREATE TRIGGER IF NOT EXISTS trg_community_ad_proposal_limit
BEFORE INSERT ON community_ad_proposals
WHEN (SELECT COUNT(*) FROM community_ad_proposals) >= 2000
BEGIN
    SELECT RAISE(ABORT, 'community ad proposal limit reached');
END;
