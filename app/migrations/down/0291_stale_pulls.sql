DROP TABLE IF EXISTS stale_pulls;
DROP TABLE IF EXISTS stale_pull_repos;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:907cb07b6f0437299d1c5030650e2de84097a943e7d5d727108abeaf297064a6';
