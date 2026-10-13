-- Preserve completed organization work without overloading the running timer
-- state used by older deployments.
ALTER TABLE world_office_marketing_tasks
  ADD COLUMN completed_at INTEGER NOT NULL DEFAULT 0
  CHECK (completed_at >= 0);

CREATE INDEX IF NOT EXISTS idx_world_office_marketing_tasks_completion
  ON world_office_marketing_tasks(org_bi, completed_at, updated_at DESC);
