-- Reverse 0193: drop the runner tier tables.
DROP TABLE IF EXISTS runner_request_nonces;
DROP INDEX IF EXISTS idx_runner_job_leases_runner;
DROP INDEX IF EXISTS idx_runner_job_leases_session;
DROP TABLE IF EXISTS runner_job_leases;
DROP TABLE IF EXISTS runner_nodes;
