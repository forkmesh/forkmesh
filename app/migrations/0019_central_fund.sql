-- Historical central donation-fund schema (issue #308).
-- Worker custody and cron signing are disabled. The active community pool uses
-- a public configured address and an owner-device signer. A pre-existing row
-- may retain an encrypted seed only until the explicit offline migration
-- exports it, reconciles its public balance, and scrubs it after confirmation.

CREATE TABLE IF NOT EXISTS central_fund (
  id   INTEGER PRIMARY KEY, -- always 1
  data TEXT NOT NULL        -- historical encrypted compatibility record
);
