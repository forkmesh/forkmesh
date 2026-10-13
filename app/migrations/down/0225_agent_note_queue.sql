DROP INDEX IF EXISTS idx_agent_note_queue_status;
DROP TABLE IF EXISTS agent_note_queue;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:a0fa4a2a85f642c250787c6ae41e6acba3816daa3b869b1cd9f76d57646a4a58';
