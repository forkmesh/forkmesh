-- Public-only Fediverse directory and authenticated shared-media rooms.
--
-- Directory `data` is intentionally plaintext because every accepted field is
-- public instance metadata. The Worker rejects private accounts/followers,
-- raw network identifiers, and relationships without explicit public/consent
-- attestations before insertion.
CREATE TABLE IF NOT EXISTS world_fediverse_instances (
    instance_id TEXT PRIMARY KEY,
    kind TEXT NOT NULL CHECK (kind IN ('mastodon', 'lemmy', 'x', 'reddit')),
    host TEXT NOT NULL UNIQUE,
    url TEXT NOT NULL UNIQUE,
    data TEXT NOT NULL,
    created_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_world_fediverse_kind_updated
    ON world_fediverse_instances(kind, updated_at);

-- A media space stores coordination metadata only. ForkMesh never stores,
-- proxies, records, or rebroadcasts media through these tables.
CREATE TABLE IF NOT EXISTS world_media_spaces (
    space_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    session_type TEXT NOT NULL CHECK (session_type IN (
        'listening-room',
        'dj-session',
        'video-room',
        'watch-party',
        'repository-launch',
        'organization-presentation'
    )),
    owner_bi TEXT NOT NULL,
    owner_label TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'active'
        CHECK (status IN ('active', 'archived')),
    playback_state TEXT NOT NULL DEFAULT 'idle'
        CHECK (playback_state IN ('idle', 'stopped')),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    last_activity_at INTEGER NOT NULL,
    archived_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_world_media_spaces_owner
    ON world_media_spaces(owner_bi, status, updated_at);
CREATE INDEX IF NOT EXISTS idx_world_media_spaces_active
    ON world_media_spaces(status, last_activity_at);

-- Owners are authoritative from world_media_spaces.owner_bi. Only explicit
-- moderator grants are added here; a platform role does not silently grant
-- room control.
CREATE TABLE IF NOT EXISTS world_media_roles (
    role_id TEXT PRIMARY KEY,
    space_id TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    account_label TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role = 'moderator'),
    granted_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE (space_id, account_bi)
);
CREATE INDEX IF NOT EXISTS idx_world_media_roles_account
    ON world_media_roles(account_bi, space_id);

-- `data` contains only a bounded title, a validated external HTTPS provider
-- page, and no-autoplay/no-rebroadcast flags. Removed entries are soft-deleted
-- long enough for moderation/audit reconciliation, then purged.
CREATE TABLE IF NOT EXISTS world_media_items (
    item_id TEXT PRIMARY KEY,
    space_id TEXT NOT NULL,
    position INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'queued'
        CHECK (status IN ('queued', 'stopped', 'removed')),
    data TEXT NOT NULL,
    added_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    removed_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_world_media_items_space
    ON world_media_items(space_id, status, position, created_at);
CREATE INDEX IF NOT EXISTS idx_world_media_items_retention
    ON world_media_items(status, removed_at);

CREATE TABLE IF NOT EXISTS world_media_schedules (
    schedule_id TEXT PRIMARY KEY,
    space_id TEXT NOT NULL,
    item_id TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'scheduled'
        CHECK (status IN ('scheduled', 'cancelled', 'completed')),
    starts_at INTEGER NOT NULL,
    ends_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    created_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    cancelled_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_world_media_schedules_space
    ON world_media_schedules(space_id, status, starts_at);
CREATE INDEX IF NOT EXISTS idx_world_media_schedules_retention
    ON world_media_schedules(status, ends_at, cancelled_at);
