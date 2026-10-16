DROP TABLE IF EXISTS badge_repo_sweeps;

DROP TABLE IF EXISTS badge_coauthor_facts;

DROP INDEX IF EXISTS idx_badge_pull_facts_author;

DROP TABLE IF EXISTS badge_pull_facts;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:c001b5e1691b7e406a0f778b87e193c27dafa246e70913476c444078966d5781';
