-- Private personal calendars and organization calendars. Event copy and
-- reminder settings are encrypted in `data`; plaintext columns are only the
-- bounded indexes needed for authorized date-range reads and reminder scans.
CREATE TABLE IF NOT EXISTS calendar_events (
  event_id TEXT PRIMARY KEY,
  owner_bi TEXT NOT NULL,
  owner_name TEXT NOT NULL,
  org_bi TEXT NOT NULL DEFAULT '',
  org_name TEXT NOT NULL DEFAULT '',
  start_at INTEGER NOT NULL,
  end_at INTEGER NOT NULL,
  all_day INTEGER NOT NULL DEFAULT 0 CHECK (all_day IN (0,1)),
  timezone TEXT NOT NULL DEFAULT 'UTC',
  recurring INTEGER NOT NULL DEFAULT 0 CHECK (recurring IN (0,1)),
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  data TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_calendar_events_owner_range
  ON calendar_events(owner_bi, start_at, end_at);
CREATE INDEX IF NOT EXISTS idx_calendar_events_org_range
  ON calendar_events(org_bi, start_at, end_at);
CREATE INDEX IF NOT EXISTS idx_calendar_events_reminders
  ON calendar_events(recurring, start_at);

CREATE TABLE IF NOT EXISTS calendar_attendees (
  event_id TEXT NOT NULL REFERENCES calendar_events(event_id) ON DELETE CASCADE,
  attendee_bi TEXT NOT NULL,
  attendee_name TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending'
    CHECK (status IN ('pending','accepted','declined','tentative')),
  created_at INTEGER NOT NULL,
  responded_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (event_id, attendee_bi)
);
CREATE INDEX IF NOT EXISTS idx_calendar_attendees_account
  ON calendar_attendees(attendee_bi, event_id);

CREATE TABLE IF NOT EXISTS calendar_reminder_deliveries (
  event_id TEXT NOT NULL REFERENCES calendar_events(event_id) ON DELETE CASCADE,
  occurrence_at INTEGER NOT NULL,
  recipient_bi TEXT NOT NULL,
  offset_minutes INTEGER NOT NULL,
  delivered_at INTEGER NOT NULL,
  PRIMARY KEY (event_id, occurrence_at, recipient_bi, offset_minutes)
);
