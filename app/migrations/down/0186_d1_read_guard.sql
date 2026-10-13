-- Reverse 0186_d1_read_guard.sql. Without the row the guard reads its
-- defaults (auto at 60%), so rolling back only forgets an operator's setting.
DROP TABLE IF EXISTS d1_read_guard;
DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:1cbcdbb6d0fe40c6553f77d3d0c3eb94b5e89cc30031abbf0ee740fac527d75d';
