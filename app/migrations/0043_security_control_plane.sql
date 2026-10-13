-- Least-privilege roles, privacy keys, abuse/quarantine controls, audit logs,
-- and non-custodial transfer intents.
--
-- No table in this migration stores a wallet private key, raw IP address, or
-- owner decryption key.

CREATE TABLE IF NOT EXISTS role_grants (
    account_bi TEXT NOT NULL,
    role TEXT NOT NULL,
    scope_type TEXT NOT NULL DEFAULT 'platform',
    scope_bi TEXT NOT NULL DEFAULT '',
    granted_by_bi TEXT,
    granted_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL DEFAULT 0,
    revoked_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (account_bi, role, scope_type, scope_bi)
);
CREATE INDEX IF NOT EXISTS idx_role_grants_role
    ON role_grants(role, scope_type, scope_bi, revoked_at, expires_at);

CREATE TABLE IF NOT EXISTS security_signals (
    subject_token TEXT NOT NULL,
    rule TEXT NOT NULL,
    reason TEXT NOT NULL,
    confidence TEXT NOT NULL,
    hits INTEGER NOT NULL DEFAULT 1,
    first_seen INTEGER NOT NULL,
    last_seen INTEGER NOT NULL,
    country_code TEXT NOT NULL DEFAULT '',
    client_category TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (subject_token, rule)
);
CREATE INDEX IF NOT EXISTS idx_security_signals_last
    ON security_signals(last_seen);

CREATE TABLE IF NOT EXISTS security_restrictions (
    incident_id TEXT PRIMARY KEY,
    subject_token TEXT NOT NULL,
    subject_kind TEXT NOT NULL DEFAULT 'network',
    reason TEXT NOT NULL,
    rule TEXT NOT NULL,
    detected_at INTEGER NOT NULL,
    duration_ms INTEGER NOT NULL,
    expires_at INTEGER NOT NULL DEFAULT 0,
    confidence TEXT NOT NULL,
    evidence_data TEXT,
    automatic INTEGER NOT NULL DEFAULT 1,
    reviewed INTEGER NOT NULL DEFAULT 0,
    reviewer_bi TEXT,
    appeal_status TEXT NOT NULL DEFAULT 'none',
    status TEXT NOT NULL DEFAULT 'quarantined',
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_security_restrictions_subject
    ON security_restrictions(subject_token, status, expires_at);
CREATE INDEX IF NOT EXISTS idx_security_restrictions_created
    ON security_restrictions(created_at);

CREATE TABLE IF NOT EXISTS security_appeals (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    incident_id TEXT NOT NULL,
    appellant_bi TEXT,
    data TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    created_at INTEGER NOT NULL,
    reviewed_at INTEGER NOT NULL DEFAULT 0,
    reviewer_bi TEXT
);
CREATE INDEX IF NOT EXISTS idx_security_appeals_incident
    ON security_appeals(incident_id, created_at);

CREATE TABLE IF NOT EXISTS sensitive_audit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    actor_bi TEXT,
    actor_label TEXT NOT NULL DEFAULT '',
    action TEXT NOT NULL,
    target_type TEXT NOT NULL DEFAULT '',
    target_token TEXT NOT NULL DEFAULT '',
    outcome TEXT NOT NULL,
    details TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS idx_sensitive_audit_ts
    ON sensitive_audit_log(ts);
CREATE INDEX IF NOT EXISTS idx_sensitive_audit_actor
    ON sensitive_audit_log(actor_bi, ts);

CREATE TABLE IF NOT EXISTS owner_encryption_keys (
    account_bi TEXT NOT NULL,
    key_id TEXT NOT NULL,
    public_bundle TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    revoked_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (account_bi, key_id)
);

CREATE TABLE IF NOT EXISTS repo_privacy_policy (
    repo_bi TEXT PRIMARY KEY,
    owner_bi TEXT NOT NULL,
    owner_key_id TEXT NOT NULL DEFAULT '',
    require_agent_e2ee INTEGER NOT NULL DEFAULT 1,
    require_mirror_encryption INTEGER NOT NULL DEFAULT 1,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS chain_intents (
    intent_id TEXT PRIMARY KEY,
    kind TEXT NOT NULL,
    source_address TEXT NOT NULL,
    signer_account TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'pending_signature',
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL DEFAULT 0,
    tx_signature TEXT NOT NULL DEFAULT '',
    completed_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_chain_intents_status
    ON chain_intents(status, created_at);
CREATE UNIQUE INDEX IF NOT EXISTS idx_chain_intents_tx_signature
    ON chain_intents(tx_signature) WHERE tx_signature<>'';

CREATE TABLE IF NOT EXISTS pending_rewards (
    reward_id TEXT PRIMARY KEY,
    recipient_bi TEXT NOT NULL,
    recipient_name TEXT NOT NULL DEFAULT '',
    source_address TEXT NOT NULL,
    amount_lamports INTEGER NOT NULL,
    wallet_address TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'pending_wallet',
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    intent_id TEXT NOT NULL DEFAULT '',
    tx_signature TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_pending_rewards_recipient
    ON pending_rewards(recipient_bi, status, expires_at);
