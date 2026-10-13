ALTER TABLE users
       ADD COLUMN early_ios_tester INTEGER NOT NULL DEFAULT 0
       CHECK (early_ios_tester IN (0, 1));

CREATE INDEX IF NOT EXISTS idx_users_early_ios_tester
  ON users(early_ios_tester, user_bi) WHERE early_ios_tester=1;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:2fd181c40f5a3e77452e7a5aca495f14385fcd13c3251b3aea1fef3d9ec8f274',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
