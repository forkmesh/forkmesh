DROP INDEX IF EXISTS idx_agent_stream_watchers_until;
DROP TABLE IF EXISTS agent_stream_watchers;
DROP TABLE IF EXISTS agent_stream_chunks;
DROP INDEX IF EXISTS idx_agent_stream_state_sweep;
DROP TABLE IF EXISTS agent_stream_state;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:ed9ec3192abcc5d97a7ec304e7e18d86d707e8b09708ad64c8afe7dbddaf410e';
