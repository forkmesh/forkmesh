-- Reverse 0175. Dropping signup_origin discards the backfilled channel; the
-- Worker-side backfill marker is removed too so a re-applied 0174 classifies
-- the encrypted signup markers again.
DROP INDEX IF EXISTS idx_product_clone_daily_day;
DROP TABLE IF EXISTS product_clone_daily;
ALTER TABLE users DROP COLUMN signup_origin;

DELETE FROM schema_meta
 WHERE k = 'schema_fingerprint:2c1fae86d4f41ca77a21db58b3ab14cee06447b33fe25a41c259043be995944d'
    OR k = 'product_analysis_signup_origin_backfill:v1';
