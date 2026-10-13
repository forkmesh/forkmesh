-- Persisted collaborative Code Workshop sessions.
--
-- Repository names, report paths, participant labels, comments, and result
-- bodies are encrypted with the Worker's established row encryption. The
-- plaintext repository blind index permits exact scoping without disclosing a
-- private repository name to a D1 operator. A commit hash and random run id do
-- not identify a repository on their own.
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

-- Membership is explicit. Platform administrators do not silently inherit
-- access; owner/editor/viewer roles are evaluated for every read and write.
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

-- Saving an update creates an immutable result id. The encrypted body contains
-- only the bounded, redacted browser analysis report.
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

-- A global integer cursor supports cheap incremental event reads. Event bodies
-- are encrypted and visible only to explicit participants.
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
