-- Reverse 0165. Rating aggregates are derived from anonymous votes and are
-- deliberately discarded when this optional public-Market feature is removed.
DROP INDEX IF EXISTS idx_package_ratings_pkg;
DROP INDEX IF EXISTS idx_package_ratings_ts;
DROP TABLE IF EXISTS package_ratings;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:1068886580e9d1a1b62add34e2a892e8a5d0daa5d07905b14695aae80e7be584';
