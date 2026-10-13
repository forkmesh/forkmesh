DROP INDEX IF EXISTS idx_organization_agent_presence_org;
DROP TABLE IF EXISTS organization_agent_presence;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:7934721f688df24fdde16249a787a69c2e46a03168a799b0274039d3af643b8b';
