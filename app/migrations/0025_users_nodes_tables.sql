-- ForkMesh D1 migration 0025 — physical users/nodes tables.
--
-- Earlier users-vs-nodes work stored the split inside encrypted `accounts.data`.
-- These tables make that model explicit for the website while preserving the
-- legacy accounts table during migration. The worker mirrors account records
-- into these tables on read/write because SQL migrations cannot decrypt and
-- reshape the encrypted account blobs by themselves.

CREATE TABLE IF NOT EXISTS users (
  user_bi  TEXT PRIMARY KEY,  -- blind_index(username)
  data     TEXT NOT NULL,     -- AES-GCM encrypted user record
  email_bi TEXT,
  username TEXT,
  is_admin INTEGER NOT NULL DEFAULT 0,
  ip_bi    TEXT
);
CREATE INDEX IF NOT EXISTS idx_users_email ON users(email_bi);
CREATE INDEX IF NOT EXISTS idx_users_ip ON users(ip_bi);

CREATE TABLE IF NOT EXISTS nodes (
  node_bi   TEXT PRIMARY KEY, -- blind_index(node name)
  user_bi   TEXT,             -- blind_index(owner username), nullable while unclaimed
  pubkey    TEXT,
  data      TEXT NOT NULL,    -- AES-GCM encrypted node record
  name      TEXT,
  last_seen INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_nodes_user ON nodes(user_bi);
CREATE INDEX IF NOT EXISTS idx_nodes_pubkey ON nodes(pubkey);
