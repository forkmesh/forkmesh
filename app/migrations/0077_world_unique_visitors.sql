-- Privacy-preserving approximate unique counts for the World Arrival Grid.
--
-- The Worker derives a keyed HMAC from Cloudflare's edge-observed source
-- address plus a coarse browser/OS/device category. Raw addresses, raw
-- User-Agent strings, and the HMAC digests never enter D1. Only HyperLogLog
-- register/rank projections are stored. Temporal HMAC context rotates each UTC
-- day and is domain-separated from the stable all-time context. One all-time
-- sketch plus at most 50 hours of 10-minute sketches bounds this table to
-- 309,248 rows (normally far fewer).
CREATE TABLE IF NOT EXISTS world_visit_unique_hll (
    bucket_start INTEGER NOT NULL,
    register_id INTEGER NOT NULL
        CHECK (register_id >= 0 AND register_id < 1024),
    rank INTEGER NOT NULL CHECK (rank >= 1 AND rank <= 247),
    PRIMARY KEY (bucket_start, register_id)
) WITHOUT ROWID;
