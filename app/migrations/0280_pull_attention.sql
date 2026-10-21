CREATE TABLE IF NOT EXISTS pull_merge_watch (
    repo_bi TEXT NOT NULL,
    number INTEGER NOT NULL,
    base_bi TEXT NOT NULL,
    head_bi TEXT NOT NULL DEFAULT '',
    due_at INTEGER NOT NULL DEFAULT 0,
    checked_at INTEGER NOT NULL DEFAULT 0,
    trial_bi TEXT NOT NULL DEFAULT '',
    notified_bi TEXT NOT NULL DEFAULT '',
    updated_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (repo_bi, number)
);
CREATE INDEX IF NOT EXISTS idx_pull_merge_watch_base ON pull_merge_watch(repo_bi, base_bi);
CREATE INDEX IF NOT EXISTS idx_pull_merge_watch_head ON pull_merge_watch(repo_bi, head_bi);
CREATE INDEX IF NOT EXISTS idx_pull_merge_watch_due ON pull_merge_watch(due_at);
CREATE INDEX IF NOT EXISTS idx_pull_merge_watch_checked ON pull_merge_watch(checked_at);
CREATE INDEX IF NOT EXISTS idx_pull_merge_watch_age ON pull_merge_watch(updated_at);

CREATE TABLE IF NOT EXISTS pull_quick_replies (
    repo_bi TEXT NOT NULL,
    number INTEGER NOT NULL,
    reviewer_bi TEXT NOT NULL,
    head TEXT NOT NULL,
    reply_id TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (repo_bi, number, reviewer_bi, head, reply_id)
);
CREATE INDEX IF NOT EXISTS idx_pull_quick_replies_age ON pull_quick_replies(created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:977a23e0537fa78fd3411ac025ee2627de0ee76931bdef5f08798217351cd78e',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
