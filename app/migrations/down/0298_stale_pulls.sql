DROP TABLE IF EXISTS stale_pulls;
DROP TABLE IF EXISTS stale_pull_repos;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:1fedcc6e32c5ad36228b7a434ebca84ce5f431fbaf6146d18d7a1e48a74137b4';
