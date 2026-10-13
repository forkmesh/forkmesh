-- Organization-private universal work. Existing Marketing tasks are copied
-- into the new catalog without decrypting or rewriting their sealed payloads,
-- so titles, details, assignees, and creator labels remain encrypted at rest.
CREATE TABLE IF NOT EXISTS organization_tasks (
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
                           'user', 'unassigned', 'claude', 'codex'
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

CREATE TABLE IF NOT EXISTS organization_task_checkins (
  checkin_id    TEXT PRIMARY KEY,
  task_id       TEXT NOT NULL,
  org_bi        TEXT NOT NULL,
  account_bi    TEXT NOT NULL,
  state         TEXT NOT NULL
                  CHECK (state IN ('going_well', 'blocked', 'needs_help')),
  data          TEXT NOT NULL,
  created_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_organization_task_checkins_task
  ON organization_task_checkins(org_bi, task_id, created_at DESC);

CREATE TABLE IF NOT EXISTS organization_task_qa_reviews (
  task_id       TEXT NOT NULL,
  org_bi        TEXT NOT NULL,
  reviewer_bi  TEXT NOT NULL,
  verdict       TEXT NOT NULL
                  CHECK (verdict IN ('passed', 'failed', 'unknown')),
  data          TEXT NOT NULL,
  reviewed_at   INTEGER NOT NULL CHECK (reviewed_at >= 0),
  PRIMARY KEY (task_id, reviewer_bi)
);
CREATE INDEX IF NOT EXISTS idx_organization_task_qa_reviews_task
  ON organization_task_qa_reviews(org_bi, task_id, reviewed_at DESC);

-- Encrypted member replies are retained separately from the task payload and
-- capped per task so task history remains useful without becoming unbounded.
CREATE TABLE IF NOT EXISTS organization_task_responses (
  response_id  TEXT PRIMARY KEY,
  task_id      TEXT NOT NULL,
  org_bi       TEXT NOT NULL,
  author_bi    TEXT NOT NULL,
  data         TEXT NOT NULL,
  created_at   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_organization_task_responses_task
  ON organization_task_responses(org_bi, task_id, created_at DESC);
CREATE TRIGGER IF NOT EXISTS trg_organization_task_response_limit
BEFORE INSERT ON organization_task_responses
WHEN (
  SELECT COUNT(*) FROM organization_task_responses
  WHERE org_bi = NEW.org_bi AND task_id = NEW.task_id
) >= 50
BEGIN
  DELETE FROM organization_task_responses
  WHERE response_id = (
    SELECT response_id FROM organization_task_responses
    WHERE org_bi = NEW.org_bi AND task_id = NEW.task_id
    ORDER BY created_at ASC, response_id ASC LIMIT 1
  );
END;

-- Limited outside collaborators are deliberately separate from org_members
-- and org_team_members. Their team placement never participates in repository
-- permission calculation or Office-floor admission.
CREATE TABLE IF NOT EXISTS org_team_collaborators (
  org_bi       TEXT NOT NULL,
  team         TEXT NOT NULL,
  account_bi   TEXT NOT NULL,
  name         TEXT NOT NULL CHECK (length(name) BETWEEN 1 AND 64),
  added_by_bi  TEXT NOT NULL,
  created_at   INTEGER NOT NULL,
  PRIMARY KEY (org_bi, team, account_bi)
);
CREATE INDEX IF NOT EXISTS idx_org_team_collaborators_account
  ON org_team_collaborators(account_bi, org_bi, team);

-- One-time promotion of every legacy Marketing record. INSERT OR IGNORE makes
-- deployment retries safe and preserves a newer universal copy if present.
INSERT OR IGNORE INTO organization_tasks (
  task_id, org_bi, department, team, destination, assignee_kind, status,
  assignee_bi, active_assignee_bi, data, created_by_bi, created_at, updated_at,
  elapsed_ms, started_at, next_checkin_at, completed_at
)
SELECT
  task_id, org_bi, 'marketing', 'marketing', 'department', 'user', status,
  assignee_bi, active_assignee_bi, data, created_by_bi, created_at, updated_at,
  elapsed_ms, started_at, next_checkin_at, completed_at
FROM world_office_marketing_tasks;

INSERT OR IGNORE INTO organization_task_checkins (
  checkin_id, task_id, org_bi, account_bi, state, data, created_at
)
SELECT checkin_id, task_id, org_bi, account_bi, state, data, created_at
FROM world_office_marketing_checkins;
