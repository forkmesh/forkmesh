DROP INDEX IF EXISTS idx_plugin_race_plugins_rank;
DROP INDEX IF EXISTS idx_plugin_race_plugins_account;
DROP TABLE IF EXISTS plugin_race_plugins;
DROP INDEX IF EXISTS idx_plugin_race_posts_account;
DROP TABLE IF EXISTS plugin_race_posts;
DROP INDEX IF EXISTS idx_plugin_race_entrants_status;
DROP TABLE IF EXISTS plugin_race_entrants;
DROP INDEX IF EXISTS idx_affiliate_leads_status;
DROP INDEX IF EXISTS idx_affiliate_leads_affiliate;
DROP INDEX IF EXISTS idx_affiliate_leads_open_domain;
DROP TABLE IF EXISTS affiliate_leads;
DROP INDEX IF EXISTS idx_affiliates_username;
DROP TABLE IF EXISTS affiliates;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:ad25ed4761784ba8ed1739f3932706abf1fc5aebe29af5eef61cce77a5bf1572';
