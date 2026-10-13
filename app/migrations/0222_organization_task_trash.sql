ALTER TABLE organization_tasks
       ADD COLUMN deleted_at INTEGER NOT NULL DEFAULT 0
       CHECK (deleted_at >= 0);

ALTER TABLE organization_tasks
       ADD COLUMN deleted_by_bi TEXT NOT NULL DEFAULT '';

CREATE INDEX IF NOT EXISTS idx_organization_tasks_trash
  ON organization_tasks(org_bi, deleted_at) WHERE deleted_at>0;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:48ef712408116a4c4a4a9ffe5ec9d3906b339ccbfcfd84c81aa783d28a3ef102',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
