-- Organization-private task replies live in a separate sealed window. The
-- trigger evicts the oldest row before admitting the next reply, so a busy
-- task can retain bounded context without an admission gate on new tasks.
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

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:8b4f4bb498e3b59b30ab3bc1c41af11badf4fdc6d159d0b52bc5defdfa39d2f8',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
