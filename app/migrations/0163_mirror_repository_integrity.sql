-- Cache a repository-scoped verification of one replaceable direct mirror
-- URL. Repository bytes and private identities remain outside D1; the row
-- contains only public reachability and signed-ref verification metadata.
CREATE TABLE IF NOT EXISTS mirror_repository_integrity (
    repo_bi TEXT NOT NULL,
    node_bi TEXT NOT NULL,
    mirror_url TEXT NOT NULL DEFAULT '',
    registration_issued_at INTEGER NOT NULL DEFAULT 0,
    reachable INTEGER NOT NULL DEFAULT 0
        CHECK (reachable IN (0, 1)),
    integrity TEXT NOT NULL DEFAULT 'unknown'
        CHECK (integrity IN ('unknown', 'ok', 'failed')),
    refs_sha256 TEXT NOT NULL DEFAULT '',
    checked_at INTEGER NOT NULL DEFAULT 0,
    latency_ms INTEGER NOT NULL DEFAULT 0,
    status_code INTEGER NOT NULL DEFAULT 0,
    reason TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (repo_bi, node_bi),
    FOREIGN KEY (node_bi) REFERENCES mirror_https_endpoints(node_bi)
        ON DELETE CASCADE
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_mirror_repository_integrity_checked
    ON mirror_repository_integrity(checked_at);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:055a3b83d984c48c11f82f4de956f87bda9bccb50f458f327db40d834a354761',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
