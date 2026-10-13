-- Make the public Office ledger reflect live occupancy instead of leaving an
-- interrupted browser session looking permanently "IN BUILDING".
--
-- The floor id is a bounded public label. last_seen_at is a server timestamp;
-- it contains no session, device, address, or user-agent data.

ALTER TABLE world_office_attendance
  ADD COLUMN last_seen_at INTEGER NOT NULL DEFAULT 0
  CHECK (last_seen_at >= 0);

ALTER TABLE world_office_attendance
  ADD COLUMN floor_id TEXT NOT NULL DEFAULT ''
  CHECK (length(floor_id) <= 32);

UPDATE world_office_attendance
SET last_seen_at = CASE
  WHEN out_at IS NULL THEN in_at
  ELSE out_at
END
WHERE last_seen_at = 0;

CREATE INDEX IF NOT EXISTS idx_world_office_attendance_live
  ON world_office_attendance(out_at, last_seen_at DESC);
