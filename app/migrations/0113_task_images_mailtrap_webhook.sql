CREATE TABLE IF NOT EXISTS organization_task_attachments (
  attachment_id TEXT PRIMARY KEY,
  task_id TEXT NOT NULL,
  org_bi TEXT NOT NULL,
  data TEXT NOT NULL,
  created_by_bi TEXT NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_organization_task_attachments_task
  ON organization_task_attachments(org_bi, task_id, created_at);

CREATE TRIGGER IF NOT EXISTS trg_organization_task_attachment_limit
BEFORE INSERT ON organization_task_attachments
WHEN (
  SELECT COUNT(*) FROM organization_task_attachments
  WHERE org_bi = NEW.org_bi AND task_id = NEW.task_id
) >= 4
BEGIN
  SELECT RAISE(ABORT, 'organization_task_attachment_catalog_full');
END;

-- Provider lifecycle state only. Addresses, subjects, and message bodies are
-- deliberately excluded; account_bi and opaque send ids are non-reversible.
CREATE TABLE IF NOT EXISTS mailtrap_email_sends (
  send_id TEXT PRIMARY KEY,
  account_bi TEXT NOT NULL,
  kind TEXT NOT NULL DEFAULT '',
  sent_at INTEGER NOT NULL,
  accepted INTEGER NOT NULL DEFAULT 0 CHECK (accepted IN (0, 1)),
  status TEXT NOT NULL DEFAULT '',
  status_at INTEGER NOT NULL DEFAULT 0,
  message_id TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_mailtrap_email_sends_account
  ON mailtrap_email_sends(account_bi, sent_at DESC);

CREATE TABLE IF NOT EXISTS mailtrap_webhook_events (
  event_id TEXT PRIMARY KEY,
  received_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_mailtrap_webhook_events_received
  ON mailtrap_webhook_events(received_at);
