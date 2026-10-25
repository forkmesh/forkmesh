DROP TABLE IF EXISTS moderation_log;
DROP TABLE IF EXISTS moderation_post_counts;
DROP TABLE IF EXISTS moderation_queue;
DROP TABLE IF EXISTS moderation_flags;
DROP TABLE IF EXISTS moderation_blocks;
DROP TABLE IF EXISTS moderation_roles;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:f2e84e44b730d394ab04d9d8d76145591e361d6d49625f9c1e5e69cfda50c1cd';
