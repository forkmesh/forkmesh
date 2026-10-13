CREATE TABLE organization_task_events_without_reminders (
  event_id    TEXT PRIMARY KEY,
  task_id     TEXT NOT NULL,
  org_bi      TEXT NOT NULL,
  actor_bi    TEXT NOT NULL,
  kind        TEXT NOT NULL
                CHECK (kind IN (
                  'created', 'updated', 'started', 'stopped', 'checkin',
                  'replied', 'completed', 'reopened', 'returned',
                  'qa_requested', 'qa_reviewed', 'deleted'
                )),
  data        TEXT NOT NULL,
  created_at  INTEGER NOT NULL
);

INSERT INTO organization_task_events_without_reminders (
  event_id, task_id, org_bi, actor_bi, kind, data, created_at
)
SELECT event_id, task_id, org_bi, actor_bi, kind, data, created_at
FROM organization_task_events
WHERE kind NOT IN ('reminded', 'reminder_seen');

DROP TABLE organization_task_events;
ALTER TABLE organization_task_events_without_reminders
  RENAME TO organization_task_events;

CREATE INDEX IF NOT EXISTS idx_organization_task_events_task
  ON organization_task_events(org_bi, task_id, created_at DESC);

CREATE TRIGGER IF NOT EXISTS trg_organization_task_event_limit
BEFORE INSERT ON organization_task_events
WHEN (
  SELECT COUNT(*) FROM organization_task_events
  WHERE org_bi = NEW.org_bi AND task_id = NEW.task_id
) >= 250
BEGIN
  DELETE FROM organization_task_events
  WHERE event_id = (
    SELECT event_id FROM organization_task_events
    WHERE org_bi = NEW.org_bi AND task_id = NEW.task_id
    ORDER BY created_at ASC, event_id ASC LIMIT 1
  );
END;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:c08eba60fa97e50441352461a248995ba2ff00f56a00f84778dc8a5a9f91ccaf';
