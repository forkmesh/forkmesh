-- Cross-isolate snapshot of the decrypted public catalog. The per-isolate
-- catalog memo could not survive isolate churn (2026-09-13: ~570 cold isolates
-- in 2.5 hours, each rescanning and re-decrypting every repositories row, the
-- single largest D1 rows-read query). A cold isolate now reads this one row,
-- trusted only under the identical catalog_rev and inside the memo's max age;
-- whichever isolate does scan publishes the result here for the rest.
-- repo_count is NULL when the scanning isolate could not count; data stays the
-- last column so count-only readers never touch its overflow pages.
CREATE TABLE IF NOT EXISTS public_catalog_snapshot (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    rev INTEGER NOT NULL DEFAULT 0,
    repo_count INTEGER,
    ts INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL DEFAULT '');

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:d639f35741cca786380f156743c8c0b3deed5f224e79d86e88b917f5cb22e1d2',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
