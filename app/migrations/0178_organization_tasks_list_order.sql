-- The organization task list orders by (completed_at>0, priority,
-- updated_at DESC, task_id DESC) with LIMIT/OFFSET. No existing index matches
-- that order, so every page sorted the organization's whole task set (~170
-- rows read per 25 returned, several hundred pages an hour while the desktop
-- Tasks page is open). This expression index lets SQLite walk the rows in
-- list order and stop at the page boundary.
CREATE INDEX IF NOT EXISTS idx_organization_tasks_list_order
  ON organization_tasks(org_bi, (completed_at>0), priority,
                        updated_at DESC, task_id DESC);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:76856bf024d6dab5f4ffd3fd7978bca030a2cc1c4d0eba228c60f9c9c50d87f0',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
