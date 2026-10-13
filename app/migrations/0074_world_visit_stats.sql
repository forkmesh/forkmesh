-- Aggregate-only Town Square arrival odometer for the Arrival Grid plaque.
-- One row per coarse 10-minute UTC bucket, plus a -1 archive row that old
-- buckets fold into. No visitor id, country, IP, or session data is stored.
CREATE TABLE IF NOT EXISTS world_visit_stats (
    bucket_start INTEGER PRIMARY KEY,
    visits INTEGER NOT NULL DEFAULT 0);
