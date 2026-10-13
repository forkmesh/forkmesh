CREATE TABLE IF NOT EXISTS world_workshop_sessions (
    session_id TEXT PRIMARY KEY,
    repo_bi TEXT NOT NULL,
    commit_hash TEXT NOT NULL,
    run_id TEXT NOT NULL,
    workshop_type TEXT NOT NULL,
    owner_bi TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'active'
        CHECK (status IN ('active', 'completed', 'archived')),
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (repo_bi, commit_hash, run_id)
);
CREATE INDEX IF NOT EXISTS idx_world_workshop_sessions_scope
    ON world_workshop_sessions(repo_bi, commit_hash, updated_at);
CREATE INDEX IF NOT EXISTS idx_world_workshop_sessions_owner
    ON world_workshop_sessions(owner_bi, status, updated_at);

CREATE TABLE IF NOT EXISTS world_workshop_participants (
    session_id TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('owner', 'editor', 'viewer')),
    data TEXT NOT NULL,
    added_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (session_id, account_bi)
);
CREATE INDEX IF NOT EXISTS idx_world_workshop_participants_account
    ON world_workshop_participants(account_bi, updated_at);

CREATE TABLE IF NOT EXISTS world_workshop_results (
    result_id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    run_id TEXT NOT NULL,
    commit_hash TEXT NOT NULL,
    created_by_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_world_workshop_results_session
    ON world_workshop_results(session_id, created_at);

CREATE TABLE IF NOT EXISTS world_workshop_events (
    event_no INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT NOT NULL UNIQUE,
    session_id TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN (
        'session-created',
        'result-saved',
        'participant-added',
        'participant-removed',
        'comment',
        'status-updated'
    )),
    actor_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_world_workshop_events_session
    ON world_workshop_events(session_id, event_no);

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:8926453f8fd31a02f4b85fcbd395fc57cbc6fe667033f57e316a35586e0c42ba';
