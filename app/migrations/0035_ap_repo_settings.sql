-- Per-repo fediverse switches, owner-managed from the web and Qt repo
-- settings: federate / broadcastEvents / acceptComments. Keyed by
-- blind_index("ap-repo-settings:<owner>/<repo>") (lowercased); data is
-- plaintext JSON of booleans so the hot unauthenticated ActivityPub gates
-- never decrypt. A missing row means "all on" (defaults).
CREATE TABLE IF NOT EXISTS ap_repo_settings (
    repo_bi TEXT PRIMARY KEY,
    data TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);
