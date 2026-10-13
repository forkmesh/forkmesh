-- Reverse 0187_ad_network.sql. Rolling back removes every submitted ad, the
-- review trail, member reports and click counters; no membership, role grant
-- or account is touched, so nothing else changes for anyone.
DROP TABLE IF EXISTS ad_network_daily_stats;
DROP TABLE IF EXISTS ad_network_reports;
DROP TABLE IF EXISTS ad_network_reviews;
DROP TABLE IF EXISTS ad_network_ads;
DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:c319a0e3054a58adc4bfd2bf492af3257396e6b759b5cda93c0252cae0fd65ea';
