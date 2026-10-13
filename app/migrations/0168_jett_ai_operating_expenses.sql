-- AI subscriptions and usage paid personally by Jett. The receipt amount is
-- already denominated in USD, so amount_minor is the authoritative
-- payment-time USD value. Each source row projects to both a historical
-- operating expense and an outstanding reimbursement debt owed to Jett.
INSERT OR IGNORE INTO operating_expenses (
    expense_id,vendor,description,paid_by_name,amount_minor,currency,
    original_amount_minor,original_currency,external_reference,
    payment_method_summary,incurred_at,recorded_at,reimbursement_status,
    reimbursed_at,reimbursement_reference
) VALUES
    (
        'openai-9ruq8rru-0005', 'OpenAI', 'OpenAI subscription invoice',
        'jett', 17527, 'USD', 0, '', '9RUQ8RRU-0005',
        '', 1784822400000, 1788130275000,
        'outstanding', 0, ''
    ),
    (
        'openai-9ruq8rru-0004', 'OpenAI', 'OpenAI subscription invoice',
        'jett', 9165, 'USD', 0, '', '9RUQ8RRU-0004',
        '', 1783180800000, 1788130275000, 'outstanding', 0, ''
    ),
    (
        'openai-9ruq8rru-0003', 'OpenAI', 'OpenAI subscription invoice',
        'jett', 2178, 'USD', 0, '', '9RUQ8RRU-0003',
        '', 1783180800000, 1788130275000, 'outstanding', 0, ''
    ),
    (
        'openai-card-2026-08-17-9507', 'OpenAI',
        'ChatGPT subscription card charge', 'jett', 9507, 'USD', 0, '',
        'OPENAI-CARD-2026-08-17-9507', '', 1786977180000,
        1788130275000, 'outstanding', 0, ''
    ),
    (
        'openai-card-2026-08-04-10533', 'OpenAI',
        'ChatGPT subscription card charge', 'jett', 10533, 'USD', 0, '',
        'OPENAI-CARD-2026-08-04-10533', '', 1785868740000,
        1788130275000, 'outstanding', 0, ''
    ),
    (
        'openai-card-2026-08-02-10000', 'OpenAI',
        'ChatGPT subscription card charge', 'jett', 10000, 'USD', 0, '',
        'OPENAI-CARD-2026-08-02-10000', '', 1785725880000,
        1788130275000, 'outstanding', 0, ''
    ),
    (
        'openai-card-2025-07-13-1089', 'OpenAI',
        'OpenAI card charge', 'jett', 1089, 'USD', 0, '',
        'OPENAI-CARD-2025-07-13-1089', '', 1752426900000,
        1788130275000, 'outstanding', 0, ''
    ),
    (
        'anthropic-8ryatvg1-0010', 'Anthropic',
        'Anthropic subscription invoice', 'jett', 21775, 'USD', 0, '',
        '8RYATVG1-0010', '', 1785081600000, 1788130275000,
        'outstanding', 0, ''
    ),
    (
        'anthropic-8ryatvg1-0009', 'Anthropic',
        'Anthropic subscription invoice', 'jett', 2178, 'USD', 0, '',
        '8RYATVG1-0009', '', 1784044800000, 1788130275000,
        'outstanding', 0, ''
    ),
    (
        'anthropic-8ryatvg1-0008', 'Anthropic',
        'Anthropic subscription invoice', 'jett', 12264, 'USD', 0, '',
        '8RYATVG1-0008', '', 1782489600000, 1788130275000,
        'outstanding', 0, ''
    ),
    (
        'anthropic-8ryatvg1-0006', 'Anthropic',
        'Anthropic subscription invoice', 'jett', 9675, 'USD', 0, '',
        '8RYATVG1-0006', '', 1782144000000, 1788130275000,
        'outstanding', 0, ''
    );
