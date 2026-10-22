DROP TABLE IF EXISTS moderation_log;
DROP TABLE IF EXISTS moderation_post_counts;
DROP TABLE IF EXISTS moderation_queue;
DROP TABLE IF EXISTS moderation_flags;
DROP TABLE IF EXISTS moderation_blocks;
DROP TABLE IF EXISTS moderation_roles;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:4be62252332156941c0bc41834fdf253cf3425aef0076a9b94841ba6a789df79';
