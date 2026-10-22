DROP TABLE IF EXISTS repo_mirror_registry;
DROP TABLE IF EXISTS repo_mirror_quota;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:f06b5d5c964d1087533f5123df26b287023f950e1144d875a20d15e9669a2a8b';
