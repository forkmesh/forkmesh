DROP TABLE IF EXISTS repo_provider_mirror_outbox;
DROP TABLE IF EXISTS repo_provider_mirror_links;
DROP TABLE IF EXISTS repo_provider_mirrors;
DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:44afdc37ea7e2a6e51da34ebec1ae83071f63f35990738c4c0f71afa5253716d';
