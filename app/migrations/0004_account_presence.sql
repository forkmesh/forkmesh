-- ForkMesh D1 migration 0004 — account-level presence for the reward split.
-- A confirmed donation is split 50% to the treasury and 50% among nodes that are
-- currently online and have a payout Solana address. "Online" = a recent heartbeat,
-- recorded here keyed by the account blind index (no plaintext node name). The
-- worker also creates this lazily (ensure_schema in src/entry.py).

CREATE TABLE IF NOT EXISTS account_presence (
  name_bi TEXT PRIMARY KEY,   -- blind_index(nodeName)
  ts      INTEGER NOT NULL    -- epoch ms of the last heartbeat
);

CREATE INDEX IF NOT EXISTS idx_account_presence_ts ON account_presence(ts);
