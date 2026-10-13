-- One-to-one direct-message conversations.
-- Pair identity and participant identity remain blind indexed, while public
-- usernames are stored only in encrypted data payloads.

CREATE TABLE IF NOT EXISTS chat_direct_conversations (
  conversation_id TEXT PRIMARY KEY,
  pair_bi         TEXT NOT NULL UNIQUE,
  data            TEXT NOT NULL,
  created_at      INTEGER NOT NULL,
  updated_at      INTEGER NOT NULL,
  key_version     INTEGER NOT NULL DEFAULT 1 CHECK (key_version >= 1)
);

CREATE TABLE IF NOT EXISTS chat_direct_participants (
  conversation_id TEXT NOT NULL,
  participant_bi  TEXT NOT NULL,
  data            TEXT NOT NULL,
  joined_at       INTEGER NOT NULL,
  PRIMARY KEY (conversation_id, participant_bi)
);

CREATE INDEX IF NOT EXISTS idx_chat_direct_participants_account
  ON chat_direct_participants(participant_bi, conversation_id);
