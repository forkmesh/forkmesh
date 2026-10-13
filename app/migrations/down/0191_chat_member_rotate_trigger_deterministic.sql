-- Reverse 0191_chat_member_rotate_trigger_deterministic.sql: restore 0069's
-- trigger, which also stamps updated_at from 'now'. Only safe on a single
-- SQLite primary (D1); on rqlite that body is frozen or diverges per replica.

DROP TRIGGER IF EXISTS trg_chat_channel_member_remove_rotate;

CREATE TRIGGER IF NOT EXISTS trg_chat_channel_member_remove_rotate
AFTER DELETE ON chat_channel_members
BEGIN
  UPDATE chat_channels
     SET key_version = key_version + 1,
         updated_at = CAST(strftime('%s','now') AS INTEGER) * 1000
   WHERE channel_id = OLD.channel_id;
END;
