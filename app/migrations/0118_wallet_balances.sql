-- Last public on-chain balance read for a member's published payout address,
-- backing the "member SOL wallets" leaderboard. Public data only: the address
-- is public profile data and the amount is a public getBalance result. The row
-- lets the board rank every published address while re-reading only a rotating
-- slice of them per rebuild.
CREATE TABLE IF NOT EXISTS wallet_balances (
    wallet TEXT PRIMARY KEY,
    name TEXT,
    lamports INTEGER NOT NULL DEFAULT 0,
    checked_at INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_wallet_balances_checked
    ON wallet_balances(checked_at);
