ALTER TABLE not_found_paths DROP COLUMN last_client;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:ce90a3db6ad2f1b24c309bcbc50b13b890c3cc605770fd4c6ae5a6a84c9cfb67';
