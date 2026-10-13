-- Private organization marketing work for the in-world Office.
--
-- Human-readable task copy, assignee labels, and check-in notes live only in
-- encrypted `data` values.  Plain columns contain opaque blind indexes,
-- bounded state, and server-authoritative timer metadata.

CREATE TABLE IF NOT EXISTS world_office_marketing_tasks (
  task_id              TEXT PRIMARY KEY,
  org_bi               TEXT NOT NULL,
  status               TEXT NOT NULL DEFAULT 'idle'
                         CHECK (status IN ('idle', 'active')),
  assignee_bi          TEXT NOT NULL,
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
                         CHECK (completed_at >= 0)
);

CREATE INDEX IF NOT EXISTS idx_world_office_marketing_tasks_org
  ON world_office_marketing_tasks(org_bi, updated_at DESC);

CREATE INDEX IF NOT EXISTS idx_world_office_marketing_tasks_assignee
  ON world_office_marketing_tasks(org_bi, assignee_bi, updated_at DESC);

-- The application uses a conditional UPDATE when starting work; this partial
-- unique index remains the race-safe final authority.
CREATE UNIQUE INDEX IF NOT EXISTS idx_world_office_one_active_per_assignee
  ON world_office_marketing_tasks(org_bi, active_assignee_bi)
  WHERE status = 'active' AND active_assignee_bi <> '';

CREATE TRIGGER IF NOT EXISTS trg_world_office_marketing_task_limit
BEFORE INSERT ON world_office_marketing_tasks
WHEN (
  SELECT COUNT(*) FROM world_office_marketing_tasks
  WHERE org_bi = NEW.org_bi
) >= 250
BEGIN
  SELECT RAISE(ABORT, 'world_office_marketing_task_catalog_full');
END;

CREATE TABLE IF NOT EXISTS world_office_marketing_checkins (
  checkin_id    TEXT PRIMARY KEY,
  task_id       TEXT NOT NULL,
  org_bi        TEXT NOT NULL,
  account_bi    TEXT NOT NULL,
  state         TEXT NOT NULL
                  CHECK (state IN ('going_well', 'blocked', 'needs_help')),
  data          TEXT NOT NULL,
  created_at    INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_world_office_marketing_checkins_task
  ON world_office_marketing_checkins(task_id, created_at DESC);

CREATE TRIGGER IF NOT EXISTS trg_world_office_marketing_checkin_limit
BEFORE INSERT ON world_office_marketing_checkins
WHEN (SELECT COUNT(*) FROM world_office_marketing_checkins) >= 12500
BEGIN
  SELECT RAISE(ABORT, 'world_office_marketing_checkin_catalog_full');
END;
