-- ForkMesh D1 migration 0039 — inbox drain log.
-- Records every time the owner's node acknowledges (drains) inbox submissions,
-- so a "my web-filed issue vanished but never synced" report can be traced:
-- which repo drained how many items, and when. The worker also creates this
-- lazily (ensure_schema in src/entry.py); this migration keeps the file-based
-- schema in sync. Content-free/operational — repo_bi is a blind index and the
-- only payload is a count, so nothing user-identifying is stored here.

CREATE TABLE IF NOT EXISTS inbox_drain_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,        -- epoch ms the drain ack landed
    repo_bi TEXT NOT NULL,      -- blind_index("<owner>/<repo>")
    kind TEXT NOT NULL,         -- 'issues' | 'pulls' | 'commits' | 'discussions'
    count INTEGER NOT NULL      -- rows removed by this drain
);

CREATE INDEX IF NOT EXISTS idx_inbox_drain_log_ts ON inbox_drain_log(ts);
CREATE INDEX IF NOT EXISTS idx_inbox_drain_log_repo ON inbox_drain_log(repo_bi);
