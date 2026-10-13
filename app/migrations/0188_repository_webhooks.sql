-- Repository webhooks (app/src/repository_webhooks.py, adhoc #207): a repo
-- owner registers a third-party https endpoint and the events it wants, and
-- the relay delivers a signed forkmesh-webhook-v1 POST when the repository is
-- pushed, a pull request is merged, or a release is published. The destination
-- URL, the HMAC signing secret and the subscribed event list live inside the
-- encrypted `data` blob; an operator reading these tables learns only that a
-- repository has a hook and how its deliveries went.

CREATE TABLE IF NOT EXISTS repo_webhooks (
    webhook_id TEXT PRIMARY KEY,
    repo_bi TEXT NOT NULL,
    active INTEGER NOT NULL DEFAULT 1,
    seed_key TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    last_status INTEGER NOT NULL DEFAULT 0,
    last_delivery_at INTEGER NOT NULL DEFAULT 0,
    failures INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL);

CREATE INDEX IF NOT EXISTS idx_repo_webhooks_repo
    ON repo_webhooks(repo_bi, created_at DESC);

-- A declared hook is materialized at most once per repository even if the
-- bookkeeping write loses a race.
CREATE UNIQUE INDEX IF NOT EXISTS idx_repo_webhooks_seed
    ON repo_webhooks(repo_bi, seed_key) WHERE seed_key <> '';

-- One row per ForkMesh-declared hook that has been materialized, so deleting a
-- seeded webhook is final instead of being undone by the next event.
CREATE TABLE IF NOT EXISTS repo_webhook_seeds (
    repo_bi TEXT NOT NULL,
    seed_key TEXT NOT NULL,
    applied_at INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, seed_key));

-- forkmesh-event-v1 orders events by a sequence that increases by exactly one
-- per observed change, so a receiver can detect a gap. One counter per
-- endpoint: an endpoint only sees the event types it subscribed to.
CREATE TABLE IF NOT EXISTS repo_webhook_sequence (
    webhook_id TEXT PRIMARY KEY,
    repo_bi TEXT NOT NULL,
    seq INTEGER NOT NULL DEFAULT 0);

CREATE INDEX IF NOT EXISTS idx_repo_webhook_sequence_repo
    ON repo_webhook_sequence(repo_bi);

-- Pending deliveries. A third-party endpoint must never add its timeout to the
-- push, merge or release request that produced the event.
CREATE TABLE IF NOT EXISTS repo_webhook_outbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    webhook_id TEXT NOT NULL,
    repo_bi TEXT NOT NULL,
    event_id TEXT NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0,
    next_ts INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    data TEXT NOT NULL);

CREATE INDEX IF NOT EXISTS idx_repo_webhook_outbox_due
    ON repo_webhook_outbox(next_ts, id);

CREATE INDEX IF NOT EXISTS idx_repo_webhook_outbox_hook
    ON repo_webhook_outbox(webhook_id);

-- The owner's delivery log, trimmed to the newest rows per hook.
CREATE TABLE IF NOT EXISTS repo_webhook_deliveries (
    delivery_id TEXT PRIMARY KEY,
    webhook_id TEXT NOT NULL,
    repo_bi TEXT NOT NULL,
    event_type TEXT NOT NULL,
    status INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0,
    outcome TEXT NOT NULL DEFAULT '',
    ts INTEGER NOT NULL);

CREATE INDEX IF NOT EXISTS idx_repo_webhook_deliveries_hook
    ON repo_webhook_deliveries(webhook_id, ts DESC);

CREATE INDEX IF NOT EXISTS idx_repo_webhook_deliveries_repo
    ON repo_webhook_deliveries(repo_bi, ts DESC);

-- The runtime schema text grew (repo_webhook*), so stamp the new fingerprint:
INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:1bef58c437e20862e3f2ffea36ea60271dd2159e262ef687094f3c62c259211d',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
