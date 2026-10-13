-- One organization-wide ordering shared by every task surface.
-- Priority 1 is highest; 999 is the bounded backlog floor.
ALTER TABLE organization_tasks
  ADD COLUMN priority INTEGER NOT NULL DEFAULT 500
  CHECK (priority BETWEEN 1 AND 999);

CREATE INDEX IF NOT EXISTS idx_organization_tasks_global_priority
  ON organization_tasks(org_bi, priority, completed_at, updated_at DESC);
