-- Server-authoritative aggregate time spent in the ForkMesh World.
--
-- account_bi is the existing keyed blind index for an account. The table has
-- no display name, IP address, route, movement history, or client-supplied
-- duration. A short-lived, server-signed World ticket is the only continuity
-- proof accepted by the Worker.
CREATE TABLE IF NOT EXISTS world_user_activity (
    account_bi TEXT PRIMARY KEY,
    total_active_ms INTEGER NOT NULL DEFAULT 0
        CHECK (total_active_ms >= 0),
    last_touch_at INTEGER NOT NULL DEFAULT 0
        CHECK (last_touch_at >= 0),
    generation INTEGER NOT NULL DEFAULT 1
        CHECK (generation > 0),
    last_credit_ms INTEGER NOT NULL DEFAULT 0
        CHECK (last_credit_ms >= 0),
    updated_at INTEGER NOT NULL DEFAULT 0
        CHECK (updated_at >= 0)
);
