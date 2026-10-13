DROP TABLE IF EXISTS not_found_ignores;

DROP INDEX IF EXISTS idx_not_found_hours_hour;

DROP TABLE IF EXISTS not_found_hours;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:4714e19ba6561716d753ff088486ac70be882ba48f1616fb2682bd69aed075d2';
