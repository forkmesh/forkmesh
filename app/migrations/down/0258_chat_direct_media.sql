DROP TABLE IF EXISTS chat_direct_media_parts;
DROP INDEX IF EXISTS idx_chat_direct_media_expiry;
DROP INDEX IF EXISTS idx_chat_direct_media_sender;
DROP INDEX IF EXISTS idx_chat_direct_media_conversation;
DROP TABLE IF EXISTS chat_direct_media;
DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:edd807e544f8e1cc4df37b41937f3efbe7188d5cce746d50fb3b0a181dea2098';
