CREATE TABLE IF NOT EXISTS pull_ai_reviews (
    repo_bi TEXT NOT NULL,
    number INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'queued',
    reason TEXT NOT NULL DEFAULT '',
    head TEXT NOT NULL DEFAULT '',
    reviewed_head TEXT NOT NULL DEFAULT '',
    head_bi TEXT NOT NULL DEFAULT '',
    attempts INTEGER NOT NULL DEFAULT 0,
    next_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (repo_bi, number)
);
CREATE INDEX IF NOT EXISTS idx_pull_ai_reviews_due ON pull_ai_reviews(status, next_at);
CREATE INDEX IF NOT EXISTS idx_pull_ai_reviews_head ON pull_ai_reviews(head_bi) WHERE head_bi <> '';
CREATE TABLE IF NOT EXISTS ai_review_budget (
    k TEXT PRIMARY KEY,
    day TEXT NOT NULL,
    count INTEGER NOT NULL
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:675ee11881d8267220fca57bd57d66aff4bef75fee99cd5f3cefd88434a74ef0',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
