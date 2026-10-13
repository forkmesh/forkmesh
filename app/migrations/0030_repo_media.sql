-- Per-repo branding: owner-uploaded logo/banner PNGs (base64 inside the
-- encrypted blob, one row per image kind so a large banner never pushes the
-- row past D1's 2 MB cap) surfaced on the repo's fediverse actor. Existence
-- checks read kind/updated_at without decrypting.
CREATE TABLE IF NOT EXISTS repo_media (
    repo_bi TEXT NOT NULL, kind TEXT NOT NULL,
    data TEXT NOT NULL, updated_at INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, kind));
