-- Business costs paid personally by Jett. USD is the exact payment-time
-- amount supplied for each charge. The canonical expense row also projects
-- an equal outstanding reimbursement debt without changing a company wallet.
INSERT OR IGNORE INTO operating_expenses (
    expense_id,vendor,description,paid_by_name,amount_minor,currency,
    original_amount_minor,original_currency,external_reference,
    payment_method_summary,incurred_at,recorded_at,reimbursement_status,
    reimbursed_at,reimbursement_reference
) VALUES
    (
        'business-registration-2026-08-26',
        'Business registration',
        'Business registration fee',
        'jett',
        20300,
        'USD',
        0,
        '',
        'BUSINESS-REGISTRATION-2026-08-26',
        '',
        1787702400000,
        1788131973000,
        'outstanding',
        0,
        ''
    ),
    (
        'apple-developer-2026-08-06',
        'Apple',
        'Apple Developer Program account',
        'jett',
        10000,
        'USD',
        0,
        '',
        'APPLE-DEVELOPER-2026-08-06',
        '',
        1785974400000,
        1788131973000,
        'outstanding',
        0,
        ''
    ),
    (
        'cloudflare-domain-2026-06-10',
        'Cloudflare',
        'ForkMesh domain registration',
        'jett',
        1046,
        'USD',
        0,
        '',
        'CLOUDFLARE-DOMAIN-2026-06-10',
        '',
        1781049600000,
        1788131973000,
        'outstanding',
        0,
        ''
    );
