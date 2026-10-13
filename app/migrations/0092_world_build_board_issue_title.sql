-- Migration 0091 already introduced the bounded public issue title.
-- Keep this migration as a no-op so databases that reached this version after
-- 0091 can record it without attempting to add the column a second time.
SELECT 1;
