DROP INDEX IF EXISTS idx_world_floor_note_replies_account;
DROP INDEX IF EXISTS idx_world_floor_note_replies_note;
DROP TABLE IF EXISTS world_floor_note_replies;
DROP INDEX IF EXISTS idx_world_floor_notes_account;
DROP INDEX IF EXISTS idx_world_floor_notes_updated;
DROP TABLE IF EXISTS world_floor_notes;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:e0c34f91c012eb7f652311c5ed1d93d5d7c09247d84340a945d94a1bff5d65b3';
