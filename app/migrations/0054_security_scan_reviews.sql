-- Human triage is stored separately from immutable scanner artifacts.
-- Repository identity remains a blind index; scan/finding ids are content
-- fingerprints, and reviewer notes/status are encrypted in `data`.

CREATE TABLE IF NOT EXISTS repo_security_scan_reviews (
    repo_bi TEXT NOT NULL,
    scan_id TEXT NOT NULL,
    finding_id TEXT NOT NULL,
    reviewer_bi TEXT NOT NULL,
    reviewed_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (repo_bi, scan_id, finding_id)
);
CREATE INDEX IF NOT EXISTS idx_repo_security_scan_reviews_scan
    ON repo_security_scan_reviews(repo_bi, scan_id, reviewed_at DESC);
