DROP INDEX IF EXISTS idx_code_challenge_members_account;
DROP TABLE IF EXISTS code_challenge_members;
DROP INDEX IF EXISTS idx_code_challenges_owner;
DROP TABLE IF EXISTS code_challenges;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:36a2234002247580a61cb258039cc1f4ebea6ba49470eba4c7b67212a7075bdb';
