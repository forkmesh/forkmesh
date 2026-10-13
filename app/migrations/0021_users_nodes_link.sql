-- ForkMesh D1 migration 0021 — users vs nodes (adhoc #53).
-- Accounts now come in two kinds, derived from the encrypted record with no
-- schema change: a "user" has login credentials (pass_hash) and may own nodes;
-- a "node" is key-bound only (e.g. a headless mirror auto-registration) and may
-- be owned by a user. The encrypted `data` blob gains `owner` (owning user's
-- name, on nodes), `nodes` (owned node names, on users), and `claim_pending`
-- (an in-flight website claim's confirmation code, on nodes).
--
-- The only new table is the installer link-code rendezvous: install.sh mints a
-- short numeric code, the fresh headless node registers with it, and the
-- installing user's desktop app offers the same code signed by its key.
-- Whichever side arrives first parks its half here; the second side completes
-- the link and deletes the row. code_bi = blind_index("link:<code>"); data is
-- an AES-GCM encrypted {node|user}; ts bounds the code's lifetime (rows are
-- pruned as they are looked up). The worker also creates this lazily
-- (ensure_schema in src/entry.py).

CREATE TABLE IF NOT EXISTS link_codes (
  code_bi TEXT PRIMARY KEY,  -- blind_index("link:" + code)
  data    TEXT NOT NULL,     -- AES-GCM encrypted {node|user}
  ts      INTEGER NOT NULL   -- creation time (ms); expires after 30 minutes
);
