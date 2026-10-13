DROP INDEX IF EXISTS idx_pull_inbox_submission;
ALTER TABLE pull_inbox DROP COLUMN submission_bi;
DROP TABLE IF EXISTS pull_submission_receipts;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:c6b023b37b488e326ace84d1d1ceead6c8e35abdeda93d24a746ab3182007690';
