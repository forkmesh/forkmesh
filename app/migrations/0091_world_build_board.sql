-- Public build-board ordering and repository-issue queue.
-- Built-in titles remain in code. Assigned issues retain their bounded,
-- already-public title plus repository/number so they remain visible after
-- leaving the recent-issue window; no issue body or private metadata is kept.

CREATE TABLE IF NOT EXISTS world_build_board_items (
  item_key       TEXT PRIMARY KEY,
  kind           TEXT NOT NULL CHECK (kind IN ('task', 'issue')),
  owner          TEXT NOT NULL DEFAULT '',
  repo           TEXT NOT NULL DEFAULT '',
  issue_number   INTEGER NOT NULL DEFAULT 0 CHECK (issue_number >= 0),
  title          TEXT NOT NULL DEFAULT '' CHECK (length(title) <= 160),
  priority       INTEGER NOT NULL CHECK (priority >= 1 AND priority <= 64),
  updated_by_bi  TEXT NOT NULL,
  updated_at     INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_world_build_board_priority
  ON world_build_board_items(priority, item_key);
