-- Reverse 0196: drop the cron-run audit and the DNS health tables.
DROP TABLE IF EXISTS dns_reconciler_state;
DROP TABLE IF EXISTS dns_health_votes;
DROP TABLE IF EXISTS host_cron_runs;
