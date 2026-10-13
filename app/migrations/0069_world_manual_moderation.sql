-- Manual, administrator-initiated moderation for the multiplayer World.
--
-- `subject_token` is a keyed, rotating opaque token derived transiently at the
-- edge. Raw IP addresses and raw user-agent strings are never stored. This
-- table does not restore automated abuse detection, quarantine, security
-- signals, appeals, or permanent restrictions.

CREATE TABLE IF NOT EXISTS world_manual_blocks (
  block_id TEXT PRIMARY KEY,
  target_type TEXT NOT NULL CHECK (target_type IN ('ip','agent')),
  subject_token TEXT NOT NULL,
  created_by_bi TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  revoked_at INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_world_manual_blocks_active
  ON world_manual_blocks(target_type, subject_token, expires_at)
  WHERE revoked_at=0;
