-- ForkMesh D1 migration 0014 — private-repo collaborator ACL (issue #9).
-- Lets an owner share a private repo with other accounts. A grantee with a row
-- here additionally sees the repo in their authenticated catalog and may clone
-- it through the relay with their OWN key (forkmesh-share-view-v1 token), while
-- the public website still shows nothing. The worker also creates this lazily
-- (ensure_schema in src/entry.py); this migration keeps the file-based schema in
-- sync. repo_bi = HMAC blind index of "<owner>/<repo>" (same key as
-- repositories.key_bi); grantee_bi = blind index of the grantee account name;
-- `data` is the AES-GCM-encrypted {grantee, owner, repo, ts} record.

CREATE TABLE IF NOT EXISTS repo_shares (
    repo_bi TEXT NOT NULL,
    grantee_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    ts INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, grantee_bi)
);
CREATE INDEX IF NOT EXISTS idx_repo_shares_grantee ON repo_shares(grantee_bi);
