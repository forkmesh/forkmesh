-- Member-to-member tip notices (self-custodial). The sender's desktop signs
-- the SOL transfer with a key held only in its own local encrypted vault and
-- broadcasts it straight to a Solana RPC; the Worker is told about the public
-- receipt afterwards so it can email the recipient. Nothing here is, or can
-- reference, private key material.
--
-- The chain signature is the primary key: replaying the same notice updates
-- one row instead of mailing the recipient again.
CREATE TABLE IF NOT EXISTS tip_notice (
    signature TEXT PRIMARY KEY CHECK (length(signature) BETWEEN 64 AND 88),
    from_bi TEXT NOT NULL,
    to_bi TEXT NOT NULL,
    lamports INTEGER NOT NULL CHECK (lamports > 0),
    network TEXT NOT NULL DEFAULT 'mainnet'
        CHECK (network IN ('mainnet','devnet','testnet')),
    ts INTEGER NOT NULL CHECK (ts >= 0),
    emailed INTEGER NOT NULL DEFAULT 0 CHECK (emailed IN (0,1))
);
CREATE INDEX IF NOT EXISTS idx_tip_notice_from_ts ON tip_notice(from_bi, ts);
CREATE INDEX IF NOT EXISTS idx_tip_notice_to_ts ON tip_notice(to_bi, ts);
