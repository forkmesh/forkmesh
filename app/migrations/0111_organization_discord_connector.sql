-- Organization-scoped Discord connector configuration.  The Worker-held bot
-- credential is intentionally absent: data holds only the selected guild and
-- public channel identifiers encrypted at rest.  No chat body or token is
-- persisted by this connector.
CREATE TABLE IF NOT EXISTS organization_discord_connectors (
  org_bi          TEXT PRIMARY KEY,
  data            TEXT NOT NULL,
  updated_by_bi   TEXT NOT NULL,
  updated_at      INTEGER NOT NULL CHECK (updated_at >= 0)
);

CREATE INDEX IF NOT EXISTS idx_organization_discord_connectors_updated
  ON organization_discord_connectors(updated_at DESC);

-- One idempotent, unassigned organization task tracks the human step that
-- currently blocks this connector.  It stores only opaque task/org ids; the
-- human-readable instructions remain in the encrypted organization_tasks row.
CREATE TABLE IF NOT EXISTS organization_discord_setup_tasks (
  org_bi          TEXT PRIMARY KEY,
  task_id         TEXT NOT NULL UNIQUE,
  created_at      INTEGER NOT NULL CHECK (created_at >= 0),
  updated_at      INTEGER NOT NULL CHECK (updated_at >= 0)
);

-- A Discord bot's global guild visibility is not organization consent. This
-- encrypted grant is created only by the owner-bound OAuth authorization-code
-- callback and has no OAuth access/refresh token, code, or raw state.
CREATE TABLE IF NOT EXISTS organization_discord_oauth_grants (
  org_bi          TEXT PRIMARY KEY,
  data            TEXT NOT NULL,
  verified_by_bi  TEXT NOT NULL,
  verified_at     INTEGER NOT NULL CHECK (verified_at >= 0),
  updated_at      INTEGER NOT NULL CHECK (updated_at >= 0)
);

CREATE INDEX IF NOT EXISTS idx_organization_discord_oauth_grants_verified
  ON organization_discord_oauth_grants(verified_at DESC);

-- `state_hash` is only a SHA-256 lookup key. The raw one-time state, PKCE
-- verifier, account/session binding, and callback transaction cookie value all
-- remain inside encrypted `data`. org_bi is metadata solely for revocation on
-- organization deletion or Disconnect.
CREATE TABLE IF NOT EXISTS organization_discord_oauth_states (
  state_hash      TEXT PRIMARY KEY,
  org_bi          TEXT NOT NULL,
  data            TEXT NOT NULL,
  created_at      INTEGER NOT NULL CHECK (created_at >= 0),
  expires_at      INTEGER NOT NULL CHECK (expires_at >= 0)
);

CREATE INDEX IF NOT EXISTS idx_organization_discord_oauth_states_org
  ON organization_discord_oauth_states(org_bi, expires_at);
