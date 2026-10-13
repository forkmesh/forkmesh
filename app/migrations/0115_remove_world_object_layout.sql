-- Remove the retired World layout editing feature.
--
-- Town Square objects are placed by the authored scene again, so the
-- administrator-curated placement overrides created by migration 0080 (and
-- extended with a heading offset by 0082) have no reader left. IF EXISTS keeps
-- this migration safe for databases where the table was never created or was
-- already removed. Sensitive audit history of past world.layout.move actions
-- intentionally remains.

DROP TABLE IF EXISTS world_object_layout;
