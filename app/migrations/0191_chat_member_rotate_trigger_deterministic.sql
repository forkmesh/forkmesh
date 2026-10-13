-- Make the chat-channel membership rotation trigger deterministic.
--
-- 0069's trigger stamped chat_channels.updated_at with
-- CAST(strftime('%s','now') AS INTEGER) * 1000. That is fine on D1 (one
-- primary executes it), but rqlite replicates SQL text, not pages:
--   * with rqlite's default rewrite, 'now' in the CREATE TRIGGER body is
--     replaced by a literal once, so every later removal stamps the time the
--     trigger was created (and, on a non-UTC host, a time shifted by the
--     host's UTC offset);
--   * without the rewrite, every replica evaluates 'now' on its own clock
--     when it applies the DELETE, and again when it replays its log, so
--     replicas can disagree on updated_at.
-- The security-relevant part, key_version + 1 in the same write as the
-- membership delete, stays in the trigger. The display timestamp moves to
-- the app (chat_channels_api._remove_member), which already knows "now".

DROP TRIGGER IF EXISTS trg_chat_channel_member_remove_rotate;

CREATE TRIGGER IF NOT EXISTS trg_chat_channel_member_remove_rotate
AFTER DELETE ON chat_channel_members
BEGIN
  UPDATE chat_channels
     SET key_version = key_version + 1
   WHERE channel_id = OLD.channel_id;
END;

-- The runtime schema text changed with it, so stamp the new fingerprint:
INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:990c0111faefe2d022f3568b34ee229a936ed71b2dadbf807ed5850a7cd46e5b',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
