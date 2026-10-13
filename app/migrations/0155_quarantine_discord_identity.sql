-- A quarantined account must retain the same stable Discord identity link as
-- its active users row so an approved restoration is lossless. The readable
-- username/display name remain inside the encrypted account record.
ALTER TABLE quarantined_users
ADD COLUMN discord_bi TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS idx_quarantined_users_discord
ON quarantined_users(discord_bi)
WHERE discord_bi IS NOT NULL AND discord_bi <> '';

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:4db7c6029ab4ec3699ef7eaa2355a198ee9e19550e6fee11e973a87c42653dd0',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
