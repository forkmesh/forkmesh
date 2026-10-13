-- The task board no longer asks whether a task goes to Claude or to Codex: it
-- carries one general bot and the Worker picks whichever runtime has an
-- eligible mirror online. Widening a CHECK constraint requires a table
-- rebuild, so the catalog is recreated with 'agent' allowed and the two legacy
-- vendor kinds are folded into it. Sealed payloads are copied verbatim, so no
-- task copy, assignee label, or check-in note is decrypted here.

DROP TRIGGER IF EXISTS trg_organization_task_limit;
DROP INDEX IF EXISTS idx_organization_tasks_org_updated;
DROP INDEX IF EXISTS idx_organization_tasks_scope;
DROP INDEX IF EXISTS idx_organization_tasks_assignee;
DROP INDEX IF EXISTS idx_organization_tasks_qa;
DROP INDEX IF EXISTS idx_organization_tasks_one_active_assignee;

ALTER TABLE organization_tasks RENAME TO organization_tasks_legacy_0108;

CREATE TABLE organization_tasks (
  task_id              TEXT PRIMARY KEY,
  org_bi               TEXT NOT NULL,
  department           TEXT NOT NULL DEFAULT 'general'
                         CHECK (length(department) BETWEEN 1 AND 64),
  team                 TEXT NOT NULL DEFAULT ''
                         CHECK (length(team) <= 64),
  destination          TEXT NOT NULL DEFAULT 'department'
                         CHECK (destination IN (
                           'department', 'personal', 'repository', 'qa',
                           'agent'
                         )),
  assignee_kind        TEXT NOT NULL DEFAULT 'user'
                         CHECK (assignee_kind IN (
                           'user', 'unassigned', 'agent'
                         )),
  status               TEXT NOT NULL DEFAULT 'idle'
                         CHECK (status IN ('idle', 'active')),
  assignee_bi          TEXT NOT NULL DEFAULT '',
  active_assignee_bi   TEXT NOT NULL DEFAULT '',
  data                 TEXT NOT NULL,
  created_by_bi        TEXT NOT NULL,
  created_at           INTEGER NOT NULL,
  updated_at           INTEGER NOT NULL,
  elapsed_ms           INTEGER NOT NULL DEFAULT 0 CHECK (elapsed_ms >= 0),
  started_at           INTEGER NOT NULL DEFAULT 0 CHECK (started_at >= 0),
  next_checkin_at      INTEGER NOT NULL DEFAULT 0
                         CHECK (next_checkin_at >= 0),
  completed_at         INTEGER NOT NULL DEFAULT 0
                         CHECK (completed_at >= 0),
  qa_status            TEXT NOT NULL DEFAULT 'unknown'
                         CHECK (qa_status IN ('unknown', 'passed', 'failed')),
  qa_reviewer_bi       TEXT NOT NULL DEFAULT '',
  qa_reviewed_at       INTEGER NOT NULL DEFAULT 0
                         CHECK (qa_reviewed_at >= 0),
  qa_requested_at      INTEGER NOT NULL DEFAULT 0
                         CHECK (qa_requested_at >= 0),
  agent_session_id     TEXT NOT NULL DEFAULT ''
                         CHECK (length(agent_session_id) <= 64)
);

INSERT INTO organization_tasks (
  task_id, org_bi, department, team, destination, assignee_kind, status,
  assignee_bi, active_assignee_bi, data, created_by_bi, created_at, updated_at,
  elapsed_ms, started_at, next_checkin_at, completed_at, qa_status,
  qa_reviewer_bi, qa_reviewed_at, qa_requested_at, agent_session_id
)
SELECT
  task_id, org_bi, department, team, destination,
  CASE
    WHEN assignee_kind IN ('claude', 'codex', 'bot') THEN 'agent'
    ELSE assignee_kind
  END,
  status,
  assignee_bi, active_assignee_bi, data, created_by_bi, created_at, updated_at,
  elapsed_ms, started_at, next_checkin_at, completed_at, qa_status,
  qa_reviewer_bi, qa_reviewed_at, qa_requested_at, agent_session_id
FROM organization_tasks_legacy_0108;

DROP TABLE organization_tasks_legacy_0108;

CREATE INDEX IF NOT EXISTS idx_organization_tasks_org_updated
  ON organization_tasks(org_bi, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_organization_tasks_scope
  ON organization_tasks(org_bi, department, team, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_organization_tasks_assignee
  ON organization_tasks(org_bi, assignee_bi, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_organization_tasks_qa
  ON organization_tasks(org_bi, qa_requested_at, qa_reviewed_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_organization_tasks_one_active_assignee
  ON organization_tasks(org_bi, active_assignee_bi)
  WHERE status = 'active' AND active_assignee_bi <> '';

CREATE TRIGGER IF NOT EXISTS trg_organization_task_limit
BEFORE INSERT ON organization_tasks
WHEN (
  SELECT COUNT(*) FROM organization_tasks WHERE org_bi = NEW.org_bi
) >= 2000
BEGIN
  SELECT RAISE(ABORT, 'organization_task_catalog_full');
END;
