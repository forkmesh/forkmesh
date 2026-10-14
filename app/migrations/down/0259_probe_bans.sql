DROP INDEX IF EXISTS idx_probe_bans_expires;
DROP TABLE IF EXISTS probe_bans;
DROP INDEX IF EXISTS idx_probe_visitors_last_seen;
DROP TABLE IF EXISTS probe_visitors;
DELETE FROM request_block_rules WHERE substr(rule_id,1,11)='auto-probe-';
