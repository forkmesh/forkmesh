-- Provider-neutral marketplace identities, anonymous per-market download
-- counts, and the two-stage community publication state machine.

-- Native migrations can run against a brand-new D1 database before the
-- Worker's lazy schema bootstrap has created this historical table.
CREATE TABLE IF NOT EXISTS package_installs (
    run TEXT NOT NULL,
    pkg TEXT NOT NULL,
    ts INTEGER NOT NULL,
    installed INTEGER NOT NULL DEFAULT 1,
    downloads INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (run, pkg)
);
DROP INDEX IF EXISTS idx_package_installs_ts;
DROP INDEX IF EXISTS idx_package_installs_pkg;
ALTER TABLE package_installs RENAME TO package_installs_legacy;
CREATE TABLE package_installs (
    run TEXT NOT NULL,
    market TEXT NOT NULL DEFAULT 'forkmesh',
    pkg TEXT NOT NULL,
    ts INTEGER NOT NULL,
    installed INTEGER NOT NULL DEFAULT 1,
    downloads INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (run, market, pkg)
);
INSERT INTO package_installs (run, market, pkg, ts, installed, downloads)
SELECT run, 'forkmesh', pkg, ts, installed, downloads
FROM package_installs_legacy;
DROP TABLE package_installs_legacy;
CREATE INDEX idx_package_installs_ts ON package_installs(ts);
CREATE INDEX idx_package_installs_pkg ON package_installs(market, pkg);

CREATE TABLE marketplace_submissions (
    submission_id TEXT PRIMARY KEY,
    marketplace_id TEXT NOT NULL,
    package_id TEXT NOT NULL,
    version TEXT NOT NULL,
    manifest_url TEXT NOT NULL,
    manifest_sha256 TEXT NOT NULL,
    sources_json TEXT NOT NULL,
    artifact_sha256 TEXT NOT NULL,
    artifact_size INTEGER NOT NULL,
    artifact_locations_json TEXT NOT NULL,
    submitted_by_bi TEXT NOT NULL,
    submitted_by_name TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'member_review',
    owner_approved_by_bi TEXT NOT NULL DEFAULT '',
    owner_approved_by_name TEXT NOT NULL DEFAULT '',
    rejection_reason TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    published_at INTEGER NOT NULL DEFAULT 0,
    UNIQUE (marketplace_id, package_id, version, manifest_sha256)
);
CREATE INDEX idx_marketplace_submissions_state
    ON marketplace_submissions(state, updated_at DESC);
CREATE INDEX idx_marketplace_submissions_package
    ON marketplace_submissions(marketplace_id, package_id, published_at DESC);

CREATE TABLE marketplace_member_approvals (
    submission_id TEXT NOT NULL,
    reviewer_bi TEXT NOT NULL,
    reviewer_name TEXT NOT NULL,
    wallet_address TEXT NOT NULL,
    value_lamports INTEGER NOT NULL DEFAULT 0,
    value_checked_at INTEGER NOT NULL DEFAULT 0,
    manifest_sha256 TEXT NOT NULL,
    approved_at INTEGER NOT NULL,
    PRIMARY KEY (submission_id, reviewer_bi),
    FOREIGN KEY (submission_id) REFERENCES marketplace_submissions(submission_id)
        ON DELETE CASCADE
);
CREATE INDEX idx_marketplace_member_approvals_value
    ON marketplace_member_approvals(
        submission_id, value_lamports DESC, approved_at
    );

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:85da831796a54b77c0aa7224bb36195e3881831f933c0c20177338553c289931',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
