CREATE TABLE IF NOT EXISTS agent_stream_state (
        session_id TEXT PRIMARY KEY,
        job_id INTEGER NOT NULL DEFAULT 0,
        job_seq INTEGER NOT NULL DEFAULT 0,
        seq INTEGER NOT NULL DEFAULT 0,
        watched_until INTEGER NOT NULL DEFAULT 0,
        ended_at INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL);

CREATE INDEX IF NOT EXISTS idx_agent_stream_state_sweep
ON agent_stream_state(ended_at, updated_at);

CREATE TABLE IF NOT EXISTS agent_stream_chunks (
        session_id TEXT NOT NULL,
        seq INTEGER NOT NULL,
        job_id INTEGER NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (session_id, seq));

CREATE TABLE IF NOT EXISTS agent_stream_watchers (
        session_id TEXT NOT NULL,
        account TEXT NOT NULL,
        until INTEGER NOT NULL,
        PRIMARY KEY (session_id, account));

CREATE INDEX IF NOT EXISTS idx_agent_stream_watchers_until
ON agent_stream_watchers(until);

-- The runtime schema text changed with it, so stamp the new fingerprint:
INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:8b94e740969a69e056513eec23989e35d44190b812559f0a0a28b0945ffa81de',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
