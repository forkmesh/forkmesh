-- One "publish a payout address" nudge per contributor, whatever they are
-- owed. A contributor reward the recipient has no wallet for is the one
-- pending state the repository owner cannot resolve themselves, so the Ledger
-- row offers to ask. Keyed on the person rather than the reward: a per-reward
-- window would let one Ledger page mail the same contributor a dozen times
-- about the same missing address.
CREATE TABLE IF NOT EXISTS reward_wallet_reminders (
    recipient_bi TEXT PRIMARY KEY,
    recipient_name TEXT NOT NULL,
    reward_id TEXT NOT NULL,
    requested_by TEXT NOT NULL,
    sent_at INTEGER NOT NULL CHECK (sent_at >= 0),
    send_count INTEGER NOT NULL DEFAULT 1
);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:d5055e5a3fef239f70ae14c9505e311f6279f8cf5c16fc43100eb6cb8854857c',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
