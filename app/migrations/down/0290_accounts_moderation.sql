DROP TABLE IF EXISTS moderation_log;
DROP TABLE IF EXISTS moderation_post_counts;
DROP TABLE IF EXISTS moderation_queue;
DROP TABLE IF EXISTS moderation_flags;
DROP TABLE IF EXISTS moderation_blocks;
DROP TABLE IF EXISTS moderation_roles;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:01dbf02dd2eaf225fcde111b1508221befaedda2ad1de70f9d646fa15bec5822';
