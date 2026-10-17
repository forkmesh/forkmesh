DELETE FROM chat_history WHERE acked_at>0;

DROP INDEX IF EXISTS idx_chat_history_acked;

ALTER TABLE chat_history DROP COLUMN acked_at;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:cfae01526d880e13fdc3ea0f18605f99244efbbc0ce71269dde05109bec15701';
