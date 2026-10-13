-- Marketing Office Hours must be derived only from explicit building punches.
-- Legacy rows remain in the lobby visit ledger but are excluded from hours.
ALTER TABLE world_office_attendance
  ADD COLUMN visit_scope TEXT NOT NULL DEFAULT 'legacy'
  CHECK (visit_scope IN ('legacy', 'office'));

CREATE INDEX IF NOT EXISTS idx_world_office_attendance_scope
  ON world_office_attendance(visit_scope, account_bi, in_at);
