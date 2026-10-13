ALTER TABLE organization_tasks DROP COLUMN progress;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:66ad83158ac88ac5ed5b8871fd1a7535fed14cc6fafe85bbdec1c2a67a0a8e62';
