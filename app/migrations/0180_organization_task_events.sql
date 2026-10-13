-- Every recorded thing that has happened to one organization task, in one
-- append-only timeline. The plaintext columns carry only the bounded event
-- kind, the blind index of the account that acted, and the timestamp; the
-- actor name, the human summary, and the @mentions it named stay sealed in
-- `data`, exactly like task copy and replies.
CREATE TABLE IF NOT EXISTS organization_task_events (
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

CREATE INDEX IF NOT EXISTS idx_organization_task_events_task
  ON organization_task_events(org_bi, task_id, created_at DESC);

-- The writer trims before inserting, so this trigger is the race-safe backstop
-- rather than the primary bound: a busy task keeps a long timeline without the
-- table ever growing without limit.
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

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:dadd1c0cdc727c578cf75d2988015de435ddbe900b5b44143d9c76a8e048e464',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
