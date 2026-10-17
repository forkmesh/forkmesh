DROP INDEX IF EXISTS idx_note_events_note;

DROP TABLE IF EXISTS note_events;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:10fa5453dc8a96ce2e6ddaaf856cad1eb790120ee6e9218b92c067248b100bf0';
