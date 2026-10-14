ALTER TABLE chat_direct_participants DROP COLUMN unread_reminded_at;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:f6b1f7f7a4bbf3af12a47342f9531f10f9c37d42ef0558873cfce3422eb4fd8f';
