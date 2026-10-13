-- Private administrator-created chat channels and direct user memberships.
-- Human-readable channel names remain inside encrypted data; name_bi only
-- enforces normalized-name uniqueness without exposing the name in plaintext.

CREATE TABLE IF NOT EXISTS chat_channels (
  channel_id    TEXT PRIMARY KEY,
  name_bi       TEXT NOT NULL UNIQUE,
  data          TEXT NOT NULL,
  created_by_bi TEXT NOT NULL,
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL,
  key_version   INTEGER NOT NULL DEFAULT 1 CHECK (key_version >= 1)
);

CREATE TABLE IF NOT EXISTS chat_channel_members (
  channel_id    TEXT NOT NULL,
  member_bi     TEXT NOT NULL,
  data          TEXT NOT NULL,
  invited_by_bi TEXT NOT NULL,
  joined_at     INTEGER NOT NULL,
  PRIMARY KEY (channel_id, member_bi)
);

CREATE INDEX IF NOT EXISTS idx_chat_channel_members_member
  ON chat_channel_members(member_bi, channel_id);

-- Revoking a real membership rotates the channel in the same SQLite write.
-- A repeated idempotent DELETE affects no row and therefore does not rotate.
CREATE TRIGGER IF NOT EXISTS trg_chat_channel_member_remove_rotate
AFTER DELETE ON chat_channel_members
BEGIN
  UPDATE chat_channels
     SET key_version = key_version + 1,
         updated_at = CAST(strftime('%s','now') AS INTEGER) * 1000
   WHERE channel_id = OLD.channel_id;
END;
