ALTER TABLE organization_tasks
       ADD COLUMN reminder_every_ms INTEGER NOT NULL DEFAULT 0
       CHECK (reminder_every_ms >= 0);

ALTER TABLE organization_tasks
       ADD COLUMN reminder_next_at INTEGER NOT NULL DEFAULT 0
       CHECK (reminder_next_at >= 0);

CREATE INDEX IF NOT EXISTS idx_organization_tasks_reminder_due
  ON organization_tasks(reminder_next_at) WHERE reminder_next_at>0;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:11bb61205cb9c0b05b1e2ffd2eba0ba60776f0d0f8b05c03744a63ed3177d90f',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
