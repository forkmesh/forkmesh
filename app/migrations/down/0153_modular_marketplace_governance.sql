-- Reverse 0153. Keep the first-party anonymous counters; federated-market
-- counters and unpublished governance state are intentionally removed.

DROP INDEX IF EXISTS idx_marketplace_member_approvals_value;
DROP TABLE IF EXISTS marketplace_member_approvals;
DROP INDEX IF EXISTS idx_marketplace_submissions_package;
DROP INDEX IF EXISTS idx_marketplace_submissions_state;
DROP TABLE IF EXISTS marketplace_submissions;

DROP INDEX IF EXISTS idx_package_installs_ts;
DROP INDEX IF EXISTS idx_package_installs_pkg;
ALTER TABLE package_installs RENAME TO package_installs_federated;
CREATE TABLE package_installs (
    run TEXT NOT NULL,
    pkg TEXT NOT NULL,
    ts INTEGER NOT NULL,
    installed INTEGER NOT NULL DEFAULT 1,
    downloads INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (run, pkg)
);
INSERT INTO package_installs (run, pkg, ts, installed, downloads)
SELECT run, pkg, ts, installed, downloads
FROM package_installs_federated
WHERE market = 'forkmesh';
DROP TABLE package_installs_federated;
CREATE INDEX idx_package_installs_ts ON package_installs(ts);
CREATE INDEX idx_package_installs_pkg ON package_installs(pkg);

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:85da831796a54b77c0aa7224bb36195e3881831f933c0c20177338553c289931';
