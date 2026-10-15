-- Anthropic invoice 8RYATVG1-0012, paid personally by Jett on 2026-09-12.
-- The receipt is already denominated in USD, so amount_minor is the
-- authoritative payment-time USD value. The row projects both a historical
-- operating expense and an outstanding reimbursement debt owed to Jett.
INSERT OR IGNORE INTO operating_expenses (
    expense_id,vendor,description,paid_by_name,amount_minor,currency,
    original_amount_minor,original_currency,external_reference,
    payment_method_summary,incurred_at,recorded_at,reimbursement_status,
    reimbursed_at,reimbursement_reference
) VALUES (
    'anthropic-8ryatvg1-0012', 'Anthropic',
    'Anthropic subscription invoice', 'jett', 21775, 'USD', 0, '',
    '8RYATVG1-0012', '', 1789171200000, 1789427462528,
    'outstanding', 0, ''
);
