-- ForkMesh D1 migration 0026 - anonymous website feedback.
-- One row per like/dislike click from static pages such as /docs.
-- The worker stores a keyed blind index of the request IP when available, not
-- the raw IP address. user_agent is retained as a legacy column name but live
-- writers store only a generalized client category. The table is bounded by
-- MAX_FEEDBACK in feedback_handler.

CREATE TABLE IF NOT EXISTS feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    source TEXT NOT NULL,
    vote TEXT NOT NULL,
    path TEXT NOT NULL,
    message TEXT,
    ip_hash TEXT,
    user_agent TEXT
);

CREATE INDEX IF NOT EXISTS idx_feedback_ts ON feedback(ts);
CREATE INDEX IF NOT EXISTS idx_feedback_source_vote ON feedback(source, vote, ts);
