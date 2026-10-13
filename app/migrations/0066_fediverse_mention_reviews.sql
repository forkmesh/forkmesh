-- Verified public fediverse repository mentions and replies shown in World.
-- Remote content, actor details, repository names, consent, and failure detail
-- live only in the encrypted data envelope. The plaintext columns are bounded
-- lifecycle/index fields and blind indexes.
CREATE TABLE IF NOT EXISTS world_fediverse_mentions (
    mention_id TEXT PRIMARY KEY,
    remote_id_bi TEXT NOT NULL UNIQUE,
    repo_bi TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('mention', 'reply')),
    state TEXT NOT NULL CHECK (state IN (
        'review', 'creating', 'pending', 'created',
        'linked', 'failed', 'dismissed'
    )),
    data TEXT NOT NULL,
    issue_number INTEGER NOT NULL DEFAULT 0 CHECK (issue_number >= 0),
    create_lease_id TEXT NOT NULL DEFAULT '',
    create_lease_until INTEGER NOT NULL DEFAULT 0,
    followup_state TEXT NOT NULL DEFAULT 'not-requested' CHECK (
        followup_state IN (
            'not-requested', 'ready', 'sending', 'sent', 'failed'
        )
    ),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_world_fediverse_mentions_feed
    ON world_fediverse_mentions(state, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_world_fediverse_mentions_repo
    ON world_fediverse_mentions(repo_bi, created_at DESC);

CREATE TABLE IF NOT EXISTS world_fediverse_mention_moderation (
    record_id TEXT PRIMARY KEY,
    mention_id TEXT NOT NULL,
    action TEXT NOT NULL CHECK (action IN ('dismiss', 'restore')),
    reason TEXT NOT NULL,
    actor_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_world_fediverse_mention_moderation
    ON world_fediverse_mention_moderation(mention_id, created_at);

