CREATE TABLE IF NOT EXISTS workflows (
  workflow_id TEXT PRIMARY KEY,
  owner_bi TEXT NOT NULL,
  active INTEGER NOT NULL DEFAULT 0,
  schedule_minutes INTEGER NOT NULL DEFAULT 0,
  next_run_at INTEGER NOT NULL DEFAULT 0,
  last_run_at INTEGER NOT NULL DEFAULT 0,
  last_status TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_workflows_owner
  ON workflows(owner_bi, updated_at);
CREATE INDEX IF NOT EXISTS idx_workflows_schedule
  ON workflows(active, next_run_at);
CREATE TABLE IF NOT EXISTS workflow_runs (
  run_id TEXT PRIMARY KEY,
  workflow_id TEXT NOT NULL,
  owner_bi TEXT NOT NULL,
  trigger_kind TEXT NOT NULL,
  status TEXT NOT NULL,
  started_at INTEGER NOT NULL,
  finished_at INTEGER NOT NULL,
  data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_workflow_runs_workflow
  ON workflow_runs(workflow_id, started_at);
CREATE INDEX IF NOT EXISTS idx_workflow_runs_owner
  ON workflow_runs(owner_bi, started_at);
CREATE TABLE IF NOT EXISTS workflow_credentials (
  credential_id TEXT PRIMARY KEY,
  owner_bi TEXT NOT NULL,
  integration TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_workflow_credentials_owner
  ON workflow_credentials(owner_bi, created_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:f9464ffd954065beb046b07ae39575394449ace82c03a8ee940a9be08907d6c6',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
