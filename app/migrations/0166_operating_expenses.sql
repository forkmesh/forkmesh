-- Canonical external operating costs. A row owns both the historical expense
-- and the reimbursement liability, so settling the member never erases or
-- duplicates the cost.
CREATE TABLE IF NOT EXISTS operating_expenses (
    expense_id TEXT PRIMARY KEY,
    vendor TEXT NOT NULL,
    description TEXT NOT NULL,
    paid_by_name TEXT NOT NULL,
    amount_minor INTEGER NOT NULL CHECK (amount_minor > 0),
    currency TEXT NOT NULL CHECK (
        length(currency) = 3 AND currency = upper(currency)),
    original_amount_minor INTEGER NOT NULL DEFAULT 0
        CHECK (original_amount_minor >= 0),
    original_currency TEXT NOT NULL DEFAULT '' CHECK (
        original_currency = '' OR (
            length(original_currency) = 3
            AND original_currency = upper(original_currency))),
    external_reference TEXT NOT NULL UNIQUE,
    payment_method_summary TEXT NOT NULL DEFAULT '',
    incurred_at INTEGER NOT NULL CHECK (incurred_at > 0),
    recorded_at INTEGER NOT NULL CHECK (recorded_at > 0),
    reimbursement_status TEXT NOT NULL DEFAULT 'outstanding'
        CHECK (reimbursement_status IN (
            'outstanding', 'reimbursed', 'waived')),
    reimbursed_at INTEGER NOT NULL DEFAULT 0 CHECK (reimbursed_at >= 0),
    reimbursement_reference TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_operating_expenses_incurred
    ON operating_expenses(incurred_at DESC, expense_id DESC);
CREATE INDEX IF NOT EXISTS idx_operating_expenses_reimbursement
    ON operating_expenses(reimbursement_status, incurred_at DESC);

-- Cursor invoice paid personally by Kaif. USD is the invoiced amount shown;
-- INR preserves the exact card charge without deriving an exchange rate.
INSERT OR IGNORE INTO operating_expenses (
    expense_id,vendor,description,paid_by_name,amount_minor,currency,
    original_amount_minor,original_currency,external_reference,
    payment_method_summary,incurred_at,recorded_at,reimbursement_status,
    reimbursed_at,reimbursement_reference
) VALUES (
    'cursor-e45e60f2-0007',
    'Cursor',
    'Cursor development subscription',
    'mdkaifansari04',
    2360,
    'USD',
    234359,
    'INR',
    'E45E60F2-0007',
    '',
    1785427200000,
    1788125816000,
    'outstanding',
    0,
    ''
);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:0b6c5f97848200f1e9194445ae1c3c09a81fb8fdb88a5e303800dcb7af68d88a',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
