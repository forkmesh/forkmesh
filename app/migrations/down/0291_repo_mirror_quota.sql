DROP TABLE IF EXISTS repo_mirror_registry;
DROP TABLE IF EXISTS repo_mirror_quota;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:3a826662aff96ff870fdb057b5cedb5c57e52adc867aff9b4d7ca8f538891cbc';
