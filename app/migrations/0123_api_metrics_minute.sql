CREATE TABLE IF NOT EXISTS api_metrics_minute (
    minute_ts INTEGER NOT NULL,
    route_group TEXT NOT NULL,
    status_class TEXT NOT NULL,
    requests INTEGER NOT NULL DEFAULT 0,
    dur_ms_sum INTEGER NOT NULL DEFAULT 0,
    dur_ms_max INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(minute_ts, route_group, status_class)
);
