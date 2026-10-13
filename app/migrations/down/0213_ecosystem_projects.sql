DROP INDEX IF EXISTS idx_ecosystem_projects_published;
DROP TABLE IF EXISTS ecosystem_projects;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:64c5461c2ec36ef0c0d86949d5591fec47362629721d4c624b1660ef51a92cca';
