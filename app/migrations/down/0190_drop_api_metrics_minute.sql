-- Reverse 0190_drop_api_metrics_minute.sql: recreate the (empty) traffic table
-- as 0123_api_metrics_minute.sql first made it and drop the failure-only table.
-- The dropped per-minute rows are not restored; they were retained for seven
-- days only.

DROP TABLE IF EXISTS flagship_read_failure_minute;

CREATE TABLE IF NOT EXISTS api_metrics_minute (
    minute_ts INTEGER NOT NULL,
    route_group TEXT NOT NULL,
    status_class TEXT NOT NULL,
    requests INTEGER NOT NULL DEFAULT 0,
    dur_ms_sum INTEGER NOT NULL DEFAULT 0,
    dur_ms_max INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(minute_ts, route_group, status_class)
);
