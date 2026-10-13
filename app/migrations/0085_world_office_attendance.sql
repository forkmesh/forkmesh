-- Shared lobby attendance for the in-world Office.
--
-- One authenticated IN punch opens one visit row; its OUT punch closes that
-- same row. The account blind index authorizes the write and the public
-- account name supplies the lobby display. Raw addresses, user-agent strings,
-- session credentials, and device identifiers are deliberately not stored.

CREATE TABLE IF NOT EXISTS world_office_attendance (
  visit_id     TEXT PRIMARY KEY CHECK (
                   length(visit_id) = 32
                   AND visit_id NOT GLOB '*[^0-9a-f]*'
                 ),
  account_bi   TEXT NOT NULL CHECK (
                   length(account_bi) = 64
                   AND account_bi NOT GLOB '*[^0-9a-f]*'
                 ),
  account_name TEXT NOT NULL CHECK (
                   length(account_name) BETWEEN 1 AND 32
                 ),
  in_at        INTEGER NOT NULL CHECK (in_at > 0),
  out_at       INTEGER CHECK (out_at IS NULL OR out_at >= in_at)
);

-- This is the race-safe idempotency boundary for duplicate background IN
-- requests: an account can have at most one open visit.
CREATE UNIQUE INDEX IF NOT EXISTS idx_world_office_attendance_open
  ON world_office_attendance(account_bi)
  WHERE out_at IS NULL;

CREATE INDEX IF NOT EXISTS idx_world_office_attendance_recent
  ON world_office_attendance(in_at DESC, visit_id DESC);
