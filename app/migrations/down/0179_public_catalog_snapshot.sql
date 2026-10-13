-- Drops the derived catalog snapshot; it is rebuilt from repositories on the
-- next catalog read, so no data is lost.
DROP TABLE IF EXISTS public_catalog_snapshot;
DELETE FROM schema_meta
 WHERE k = 'schema_fingerprint:d639f35741cca786380f156743c8c0b3deed5f224e79d86e88b917f5cb22e1d2';
