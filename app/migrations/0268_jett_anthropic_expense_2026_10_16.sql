INSERT OR IGNORE INTO operating_expenses (
    expense_id,vendor,description,paid_by_name,amount_minor,currency,
    original_amount_minor,original_currency,external_reference,
    payment_method_summary,incurred_at,recorded_at,reimbursement_status,
    reimbursed_at,reimbursement_reference
) VALUES (
    'anthropic-8ryatvg1-0013', 'Anthropic',
    'Anthropic subscription invoice', 'jett', 21775, 'USD', 0, '',
    '8RYATVG1-0013', '', 1792108800000, 1792172288341,
    'outstanding', 0, ''
);
