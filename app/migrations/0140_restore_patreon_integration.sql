-- Restore the Patreon OAuth/link/reconciliation tables used by the recovered
-- runtime. Provider credentials and raw Patreon identifiers are never stored:
-- durable identity lookups use blind indexes and display state is encrypted.

CREATE TABLE IF NOT EXISTS patreon_oauth_states (
  state_hash TEXT PRIMARY KEY,
  account_bi TEXT NOT NULL,
  data TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_patreon_oauth_states_account_expiry
  ON patreon_oauth_states(account_bi, expires_at);

CREATE TABLE IF NOT EXISTS patreon_account_links (
  account_bi TEXT PRIMARY KEY,
  patreon_user_bi TEXT NOT NULL UNIQUE,
  data TEXT NOT NULL,
  linked_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS patreon_webhook_events (
  event_id TEXT PRIMARY KEY,
  received_at INTEGER NOT NULL,
  event_type TEXT NOT NULL,
  account_bi TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_patreon_webhook_events_received
  ON patreon_webhook_events(received_at);

CREATE TABLE IF NOT EXISTS patreon_reconciliation_generations (
  member_bi TEXT PRIMARY KEY,
  generation INTEGER NOT NULL CHECK (generation >= 1),
  updated_at INTEGER NOT NULL CHECK (updated_at >= 0)
);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:5ccebd7a36672762cdd688e677552f246774680fcbf8829a6a1087ca68ca8072',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
