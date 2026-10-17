DROP INDEX IF EXISTS idx_note_events_note;

DROP TABLE IF EXISTS note_events;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:1376a9e04476130360fe7ebf9fcb641b8288b2e5010ee038a255437fff2df918';
