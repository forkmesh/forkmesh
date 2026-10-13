-- Link a ForkMesh user to the Discord account that reports their bugs.
--
-- Reporter credit on an issue is its unsigned authorName. Which ForkMesh
-- account a Discord reporter owns was not stored anywhere, so each triage pass
-- re-derived it from chat history and one pass guessed wrong. This puts it in
-- the database next to the other identity columns.
--
-- discord_bi is the blind index of the Discord *user id* (the snowflake), not
-- the handle: handles are renameable, snowflakes are not. The readable handle
-- lives inside the encrypted data blob beside mastodon/solana; no plaintext
-- Discord identifier is stored in a column, matching email_bi and ip_bi.
--
-- Unique where set: one Discord account belongs to exactly one ForkMesh user,
-- so a second link fails loudly instead of silently re-crediting someone else.
ALTER TABLE users ADD COLUMN discord_bi TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS idx_users_discord
  ON users(discord_bi) WHERE discord_bi IS NOT NULL AND discord_bi <> '';

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:8bbb45e6eb0c478af85e1982a4be9857aaf220e959f9bd39d1e1c498e3d6cdbf',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
