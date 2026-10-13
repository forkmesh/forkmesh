-- Privacy deletions are deliberately not reversible: a rollback must never
-- recreate payment-card identifiers that ForkMesh no longer needs.
UPDATE operating_expenses
SET payment_method_summary=''
WHERE payment_method_summary<>'';
