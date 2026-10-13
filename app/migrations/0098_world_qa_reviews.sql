-- Per-account World QA deck progress. The blind index is the service's opaque
-- account key; connection metadata and free-form content are never stored.
CREATE TABLE IF NOT EXISTS world_qa_reviews (
    account_bi TEXT NOT NULL,
    item_key TEXT NOT NULL CHECK (length(item_key) BETWEEN 1 AND 80),
    verdict TEXT NOT NULL CHECK (verdict IN ('pass','fail','unsure')),
    reviewed_at INTEGER NOT NULL CHECK (reviewed_at >= 0),
    PRIMARY KEY (account_bi, item_key)
);

CREATE INDEX IF NOT EXISTS idx_world_qa_reviews_account_time
    ON world_qa_reviews(account_bi, reviewed_at DESC);
