DROP TABLE IF EXISTS security_advisory_settings;
DROP TABLE IF EXISTS security_advisory_credits;
DROP TABLE IF EXISTS security_advisory_comments;
DROP TABLE IF EXISTS security_advisory_members;
DROP TABLE IF EXISTS security_advisories;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:33b110eeae047152e57f209197a0f7c659442de5bb7c804713ccad3a5a7ac80e';
