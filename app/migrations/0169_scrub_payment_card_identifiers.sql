-- Payment-card brand and last-four details are unnecessary for public
-- accounting. Remove every stored summary; invoice/source references remain
-- sufficient to reconcile each expense without publishing card identifiers.
UPDATE operating_expenses
SET payment_method_summary=''
WHERE payment_method_summary<>'';
