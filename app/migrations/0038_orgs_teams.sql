-- Organizations + teams (issue #388): user-created org namespaces that serve
-- linked repos at /<org>/<repo> instead of the hosting node's name, with teams
-- granting members read/write/maintain/admin over the org's repositories.
--
-- Org identity and membership rosters are public data (the same trust level as
-- accounts.name / profile_follows / outreach_team), so names and roles stay
-- plaintext for cheap listing; per-org extras (display name, description,
-- creator) live in the encrypted `data` blob. org_bi = blind_index("org:<name>").
CREATE TABLE IF NOT EXISTS orgs (
    org_bi TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL);

-- Who belongs to an org and at what role ('owner' | 'admin' | 'member').
-- member_bi = blind_index(account name); `name` is the public account name.
CREATE TABLE IF NOT EXISTS org_members (
    org_bi TEXT NOT NULL,
    member_bi TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'member',
    name TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (org_bi, member_bi));
CREATE INDEX IF NOT EXISTS idx_org_members_member ON org_members(member_bi);

-- Teams: a named subset of org members holding one repository permission
-- ('read' | 'write' | 'maintain' | 'admin') over the org's linked repos.
CREATE TABLE IF NOT EXISTS org_teams (
    org_bi TEXT NOT NULL,
    team TEXT NOT NULL,
    permission TEXT NOT NULL DEFAULT 'read',
    created_at INTEGER NOT NULL,
    PRIMARY KEY (org_bi, team));

CREATE TABLE IF NOT EXISTS org_team_members (
    org_bi TEXT NOT NULL,
    team TEXT NOT NULL,
    member_bi TEXT NOT NULL,
    name TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (org_bi, team, member_bi));

-- Alias map: which node-hosted repo each /<org>/<repo> URL serves. node_owner
-- is the (public catalog) account name of the node holding the working copy;
-- the reverse index lets the push gate find the orgs a node repo is linked
-- under without scanning.
CREATE TABLE IF NOT EXISTS org_repos (
    org_bi TEXT NOT NULL,
    repo TEXT NOT NULL,
    node_owner TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (org_bi, repo));
CREATE INDEX IF NOT EXISTS idx_org_repos_node ON org_repos(node_owner, repo);
