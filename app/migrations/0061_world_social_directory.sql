-- Extend the public/consent-attested directory with X and Reddit centers.
--
-- SQLite cannot widen a CHECK constraint in place. Rebuilding this metadata-
-- only table retains existing Mastodon/Lemmy rows while allowing only the two
-- additional enumerated kinds. Application validation still requires explicit
-- per-profile public/consent attestations and stores no OAuth tokens, private
-- graph data, email addresses, or remote private account identifiers.
CREATE TABLE world_social_directory_v3 (
    instance_id TEXT PRIMARY KEY,
    kind TEXT NOT NULL CHECK (kind IN ('mastodon', 'lemmy', 'x', 'reddit')),
    host TEXT NOT NULL UNIQUE,
    url TEXT NOT NULL UNIQUE,
    data TEXT NOT NULL,
    created_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

INSERT INTO world_social_directory_v3
    (instance_id, kind, host, url, data, created_by_bi, created_at, updated_at)
SELECT instance_id, kind, host, url, data, created_by_bi, created_at, updated_at
FROM world_fediverse_instances;

DROP TABLE world_fediverse_instances;
ALTER TABLE world_social_directory_v3 RENAME TO world_fediverse_instances;

CREATE INDEX idx_world_fediverse_kind_updated
    ON world_fediverse_instances(kind, updated_at);
