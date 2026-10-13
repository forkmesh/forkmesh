-- Last-known-good public CelesTrak VISUAL OMM snapshot for the World sky.
--
-- The fixed CHECK on snapshot_id makes this a single-row cache. Orbital
-- elements are public; this table contains no visitor, location, account,
-- session, repository, or wallet data.
CREATE TABLE IF NOT EXISTS world_satellite_snapshot (
    snapshot_id INTEGER PRIMARY KEY CHECK (snapshot_id = 1),
    data TEXT NOT NULL CHECK (
        length(data) > 0 AND length(data) <= 524288
    ),
    digest TEXT NOT NULL CHECK (
        length(digest) = 64
        AND digest NOT GLOB '*[^0-9a-f]*'
    ),
    fetched_at INTEGER NOT NULL CHECK (fetched_at > 0),
    source_epoch TEXT NOT NULL,
    last_attempt_at INTEGER NOT NULL CHECK (last_attempt_at > 0),
    last_status INTEGER NOT NULL DEFAULT 200 CHECK (
        last_status >= 0 AND last_status <= 599
    ),
    last_error TEXT NOT NULL DEFAULT '' CHECK (length(last_error) <= 240)
);
