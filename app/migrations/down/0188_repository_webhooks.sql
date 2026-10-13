-- Reverse 0188_repository_webhooks.sql. Rolling back removes every registered
-- repository webhook, its queued deliveries and its delivery log; no
-- repository, account or mirror state is touched, so nothing else changes.
-- Third-party endpoints simply stop receiving events.
DROP TABLE IF EXISTS repo_webhook_deliveries;
DROP TABLE IF EXISTS repo_webhook_outbox;
DROP TABLE IF EXISTS repo_webhook_seeds;
DROP TABLE IF EXISTS repo_webhook_sequence;
DROP INDEX IF EXISTS idx_repo_webhooks_seed;
DROP TABLE IF EXISTS repo_webhooks;
DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:1bef58c437e20862e3f2ffea36ea60271dd2159e262ef687094f3c62c259211d';
