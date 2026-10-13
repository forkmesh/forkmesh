CREATE TABLE IF NOT EXISTS code_challenges (
  challenge_id TEXT PRIMARY KEY,
  owner_bi TEXT NOT NULL,
  owner_name TEXT NOT NULL,
  starts_on TEXT NOT NULL,
  ends_on TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  data TEXT NOT NULL DEFAULT '{}'
);

CREATE INDEX IF NOT EXISTS idx_code_challenges_owner
  ON code_challenges(owner_bi, ends_on);

CREATE TABLE IF NOT EXISTS code_challenge_members (
  challenge_id TEXT NOT NULL,
  account_bi TEXT NOT NULL,
  name TEXT NOT NULL,
  joined_at INTEGER NOT NULL,
  PRIMARY KEY (challenge_id, account_bi)
);

CREATE INDEX IF NOT EXISTS idx_code_challenge_members_account
  ON code_challenge_members(account_bi);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:36a2234002247580a61cb258039cc1f4ebea6ba49470eba4c7b67212a7075bdb',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
