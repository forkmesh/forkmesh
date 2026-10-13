-- Related user for each logged error.
--
-- The admin error view names the account whose own session was making the
-- request that failed, so a recurring failure can be traced to the affected
-- user instead of only to a path. Anonymous traffic stores '' — the value is
-- never taken from a caller-supplied name, only from a verified session.
ALTER TABLE error_log ADD COLUMN actor TEXT NOT NULL DEFAULT '';
