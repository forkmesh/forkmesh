ALTER TABLE mirror_https_endpoints DROP COLUMN source_ip;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:4af287707e32223803b7af9a0f97fb901c47f2d95cb5543c4e70addbe1a7a687';
