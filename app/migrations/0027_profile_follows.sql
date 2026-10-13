-- ForkMesh D1 migration 0027 - public profile follow graph.
-- Rows are keyed by blind indexes for indexed lookups while storing the public
-- profile names needed for display and namespace rename maintenance.

CREATE TABLE IF NOT EXISTS profile_follows (
    follower_bi TEXT NOT NULL,
    target_bi TEXT NOT NULL,
    follower_name TEXT NOT NULL,
    target_name TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (follower_bi, target_bi)
);

CREATE INDEX IF NOT EXISTS idx_profile_follows_target
    ON profile_follows(target_bi, created_at);
CREATE INDEX IF NOT EXISTS idx_profile_follows_follower
    ON profile_follows(follower_bi, created_at);
