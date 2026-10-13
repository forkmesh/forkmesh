DROP INDEX IF EXISTS idx_chat_read_marks_updated;
DROP TABLE IF EXISTS chat_read_marks;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:8b9cfecc9dc7c14451ee533a2f4c819b8293f164b53f60d5799e06e9b084dbe3';
