-- ForkMesh-hosted repositories on the Next core nodes (hosted_repos.py,
-- server/hosted.go, docs/HOSTED_REPOS.md).
--
-- hosted_repos        one repository created on the website and hosted by
--                     every core node. data is encrypted ({owner, name,
--                     organization, aliases, init, createdBy}); key_bi is the
--                     catalog key, creator_bi the account whose quota it uses.
--                     pushed_at/pushed_by name the last push and the core that
--                     accepted it; the others fetch until they report it.
--                     deleted_at is set once the catalog row is gone; the
--                     cores move their copies to .trash for 30 days after.
-- hosted_repo_copies  what each core holds, written only by that core through
--                     the signed replica exchange (leaderless).
--
-- Host-level tables like 0196's: not part of the Worker's runtime schema text,
-- so the schema fingerprint is unchanged.

CREATE TABLE IF NOT EXISTS hosted_repos (
        id TEXT PRIMARY KEY,
        key_bi TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        creator_bi TEXT NOT NULL,
        visibility TEXT NOT NULL CHECK (visibility IN ('public','private')),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        pushed_at INTEGER NOT NULL DEFAULT 0,
        pushed_by TEXT NOT NULL DEFAULT '',
        deleted_at INTEGER NOT NULL DEFAULT 0);

CREATE UNIQUE INDEX IF NOT EXISTS idx_hosted_repos_key
ON hosted_repos(key_bi) WHERE deleted_at=0;

CREATE INDEX IF NOT EXISTS idx_hosted_repos_creator
ON hosted_repos(creator_bi, deleted_at);

CREATE TABLE IF NOT EXISTS hosted_repo_copies (
        repo_id TEXT NOT NULL,
        node TEXT NOT NULL,
        pushed_at INTEGER NOT NULL DEFAULT 0,
        empty INTEGER NOT NULL DEFAULT 1,
        routing_digest TEXT NOT NULL DEFAULT '',
        size_bytes INTEGER NOT NULL DEFAULT 0,
        seen_at INTEGER NOT NULL,
        PRIMARY KEY (repo_id, node));
