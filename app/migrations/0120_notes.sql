-- Encrypted collaborative Markdown notes with simple snapshots and ACLs.
CREATE TABLE IF NOT EXISTS notes (
    note_id TEXT PRIMARY KEY CHECK (length(note_id) = 32),
    owner_bi TEXT NOT NULL,
    visibility TEXT NOT NULL DEFAULT 'private'
        CHECK (visibility IN ('private','public')),
    version INTEGER NOT NULL DEFAULT 1 CHECK (version >= 1),
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    published_at INTEGER NOT NULL DEFAULT 0 CHECK (published_at >= 0),
    data TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_notes_owner_updated
    ON notes(owner_bi, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_notes_public_updated
    ON notes(visibility, updated_at DESC);

CREATE TABLE IF NOT EXISTS note_shares (
    note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
    principal_type TEXT NOT NULL
        CHECK (principal_type IN ('user','organization')),
    principal_bi TEXT NOT NULL,
    principal_name TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('viewer','editor')),
    added_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    PRIMARY KEY(note_id, principal_type, principal_bi)
);
CREATE INDEX IF NOT EXISTS idx_note_shares_principal
    ON note_shares(principal_type, principal_bi, note_id);

CREATE TABLE IF NOT EXISTS note_versions (
    note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
    version INTEGER NOT NULL CHECK (version >= 1),
    author_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    data TEXT NOT NULL,
    PRIMARY KEY(note_id, version)
);

CREATE TABLE IF NOT EXISTS note_links (
    note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
    owner TEXT NOT NULL,
    repo TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('issue','pull','discussion')),
    number INTEGER NOT NULL CHECK (number BETWEEN 1 AND 999999999),
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    PRIMARY KEY(note_id, owner, repo, kind, number)
);
CREATE INDEX IF NOT EXISTS idx_note_links_target
    ON note_links(owner, repo, kind, number, note_id);
