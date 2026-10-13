ALTER TABLE chat_direct_conversations
  ADD COLUMN message_count INTEGER NOT NULL DEFAULT 0
  CHECK (message_count >= 0);

ALTER TABLE chat_direct_participants
  ADD COLUMN last_read_count INTEGER NOT NULL DEFAULT 0
  CHECK (last_read_count >= 0);

ALTER TABLE chat_direct_participants
  ADD COLUMN initiated INTEGER NOT NULL DEFAULT 0
  CHECK (initiated IN (0, 1));

CREATE INDEX IF NOT EXISTS idx_chat_direct_participants_creation_rate
  ON chat_direct_participants(participant_bi, initiated, joined_at);

CREATE TRIGGER IF NOT EXISTS trg_chat_direct_creation_rate
BEFORE INSERT ON chat_direct_participants
WHEN NEW.initiated = 1 AND (
  SELECT COUNT(*)
  FROM chat_direct_participants
  WHERE participant_bi = NEW.participant_bi
    AND initiated = 1
    AND joined_at > NEW.joined_at - 3600000
) >= 20
BEGIN
  SELECT RAISE(ABORT, 'chat_direct_creation_rate_limited');
END;
