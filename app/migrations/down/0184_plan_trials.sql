-- Reverse 0184_plan_trials.sql. Dropping the register loses trial history
-- only; it does not revoke the promotion-scope role grants trials wrote, and
-- those expire on their own, so no in-flight access changes on rollback.
DROP TABLE IF EXISTS plan_trials;
DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:bb64b9830dd4dc29d9f6e4cb28245d2b9e375511b38d686094b0df464db41910';
