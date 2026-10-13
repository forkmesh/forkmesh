DROP INDEX IF EXISTS idx_org_office_visits_started;
DROP TABLE IF EXISTS org_office_visits;
DROP TABLE IF EXISTS org_office_reads;
DROP INDEX IF EXISTS idx_org_office_dms_member_b;
DROP INDEX IF EXISTS idx_org_office_dms_member_a;
DROP TABLE IF EXISTS org_office_dms;
DROP INDEX IF EXISTS idx_org_office_messages_expires;
DROP INDEX IF EXISTS idx_org_office_messages_channel;
DROP TABLE IF EXISTS org_office_messages;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:8b6c53c28c6058afd79fb9aa15677b7baa746794a6406fe35eed5d9bd818a1ab';
