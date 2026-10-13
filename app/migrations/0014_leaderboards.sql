-- ForkMesh D1 migration 0014 — leaderboard backing data (issue #11).
-- Backs the public ranking boards on /network/ and the desktop client's
-- Leaderboards view. The worker also creates these lazily (ensure_schema in
-- src/entry.py); this migration keeps the file-based schema in sync.
--
-- Privacy: repo_first_hosted keys on the same blind index host_presence uses
-- (no plaintext repo name). contributor_activity stores the contributor's
-- public author name (issue/PR authorship is already public). funds_received
-- records lamports disbursed to each payout recipient; treasury transfers are
-- never recorded here.

-- First time each repo's tunnel was ever seen live (for "longest hosted").
-- Written once and never updated, so it survives the host going offline.
CREATE TABLE IF NOT EXISTS repo_first_hosted (
    repo_bi TEXT PRIMARY KEY,   -- blind index == repositories.key_bi for catalog repos
    ts INTEGER NOT NULL         -- epoch ms first hosted
);

-- Cumulative contributor activity (issues + PRs + commits), tallied as signed
-- events are accepted into the inbox. The inbox tables get drained on merge, so
-- the running totals live here.
CREATE TABLE IF NOT EXISTS contributor_activity (
    author_bi TEXT PRIMARY KEY, -- blind index of the lowercased author name
    name TEXT NOT NULL,         -- plaintext display name (public authorship)
    issues INTEGER NOT NULL DEFAULT 0,
    pulls INTEGER NOT NULL DEFAULT 0,
    commits INTEGER NOT NULL DEFAULT 0,
    total INTEGER NOT NULL DEFAULT 0,
    last_ts INTEGER             -- epoch ms of the most recent contribution
);
CREATE INDEX IF NOT EXISTS idx_contributor_activity_total ON contributor_activity(total);

-- Cumulative lamports disbursed to each recipient (for the "funds received"
-- boards). scope is 'mainnode' (donation-sweep node split), 'contributor'
-- (bounty payee), or 'project' (the owner/repo a paid bounty belonged to).
-- key is the recipient's Solana address (mainnode/contributor) or "owner/repo"
-- (project).
CREATE TABLE IF NOT EXISTS funds_received (
    scope TEXT NOT NULL,        -- mainnode | contributor | project
    key TEXT NOT NULL,          -- wallet address or owner/repo
    name TEXT,                  -- friendly display label, if known
    lamports INTEGER NOT NULL DEFAULT 0,
    last_ts INTEGER,            -- epoch ms of the most recent credit
    PRIMARY KEY (scope, key)
);
