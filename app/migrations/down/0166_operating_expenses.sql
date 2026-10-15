DROP INDEX IF EXISTS idx_operating_expenses_reimbursement;
DROP INDEX IF EXISTS idx_operating_expenses_incurred;
DROP TABLE IF EXISTS operating_expenses;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:0b6c5f97848200f1e9194445ae1c3c09a81fb8fdb88a5e303800dcb7af68d88a';
