DROP TABLE IF EXISTS security_advisory_settings;
DROP TABLE IF EXISTS security_advisory_credits;
DROP TABLE IF EXISTS security_advisory_comments;
DROP TABLE IF EXISTS security_advisory_members;
DROP TABLE IF EXISTS security_advisories;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:c42d16ef4a1708560657263b86ef151172e3f35dae0b14f9f8ae36fcc7562e21';
