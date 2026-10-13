DROP INDEX IF EXISTS idx_chat_channel_reads_account;
DROP TABLE IF EXISTS chat_channel_reads;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:1c97e9dd0950cb536895dd5ef9dfa241b695d23555d31be5226ad29541a996c2';
