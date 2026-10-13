DROP INDEX IF EXISTS idx_not_found_paths_alerted;

DROP INDEX IF EXISTS idx_not_found_paths_hits;

DROP INDEX IF EXISTS idx_not_found_paths_last_seen;

DROP TABLE IF EXISTS not_found_paths;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:1075c3d7eb563531fd057a494f33b83f52b0c3e91be4ef68bc0a66b51b9dd75f';
