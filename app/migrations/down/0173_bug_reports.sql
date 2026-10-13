DROP INDEX IF EXISTS idx_bug_reports_status;
DROP INDEX IF EXISTS idx_bug_reports_reporter;
DROP INDEX IF EXISTS idx_bug_reports_created;
DROP TABLE IF EXISTS bug_reports;
DELETE FROM schema_meta
 WHERE k = 'schema_fingerprint:8ca6018a61314587ee81b58b935c65cebe5174ea5ece9f4e86cd85c799db9774';
