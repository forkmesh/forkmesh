-- D1 read guard switch. Heavy D1 readers share one colo-wide cached copy
-- (app/src/d1_read_guard.py); this single row decides when they conserve:
-- auto once today's sampled rows read reach the threshold percent, on always,
-- off never. Set from the desktop Log -> D1 tab.

CREATE TABLE IF NOT EXISTS d1_read_guard (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    mode TEXT NOT NULL DEFAULT 'auto' CHECK (mode IN ('auto', 'on', 'off')),
    threshold INTEGER NOT NULL DEFAULT 60
        CHECK (threshold BETWEEN 1 AND 100),
    updated_at INTEGER NOT NULL DEFAULT 0,
    updated_by TEXT NOT NULL DEFAULT ''
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:1cbcdbb6d0fe40c6553f77d3d0c3eb94b5e89cc30031abbf0ee740fac527d75d',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
