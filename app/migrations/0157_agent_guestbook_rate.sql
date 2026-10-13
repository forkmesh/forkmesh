-- Narrow unauthenticated AI guestbook write budget. The primary key is a
-- DATA_KEY-blinded requester address; no raw address, agent name, user name, or
-- message is stored here. Guestbook content uses ordinary encrypted #general
-- history rather than a second AI-specific message store.
CREATE TABLE IF NOT EXISTS agent_guestbook_rate (
    visitor_bi TEXT PRIMARY KEY,
    count INTEGER NOT NULL DEFAULT 0,
    window_start_ts INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_agent_guestbook_rate_window
ON agent_guestbook_rate(window_start_ts);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:4273f4cba21862087cf50167d9f9e14bd722b7205d79aa6e39978e20d18bf4e7',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
