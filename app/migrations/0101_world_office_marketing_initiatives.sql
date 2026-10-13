-- Repository issues promoted into the private Marketing initiatives wall.
--
-- Issue titles and repository coordinates are sealed. Only opaque organization
-- and source indexes plus bounded timestamps remain in plaintext.
CREATE TABLE IF NOT EXISTS world_office_marketing_initiatives (
    initiative_id TEXT PRIMARY KEY CHECK (
        length(initiative_id) = 32
        AND initiative_id NOT GLOB '*[^0-9a-f]*'
    ),
    org_bi TEXT NOT NULL,
    source_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    created_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at > 0),
    UNIQUE(org_bi, source_bi)
);

CREATE INDEX IF NOT EXISTS idx_world_office_marketing_initiatives_org
ON world_office_marketing_initiatives(
    org_bi, created_at DESC, initiative_id DESC
);

CREATE TRIGGER IF NOT EXISTS trg_world_office_marketing_initiatives_capacity
BEFORE INSERT ON world_office_marketing_initiatives
WHEN (SELECT COUNT(*) FROM world_office_marketing_initiatives) >= 5000
BEGIN
    SELECT RAISE(ABORT, 'world_office_marketing_initiative_catalog_full');
END;
