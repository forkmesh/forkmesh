-- Add one privacy-preserving lifetime step counter to the existing World
-- activity aggregate. The Durable Object writes only whole-step increments;
-- coordinates, routes, and individual movement samples remain ephemeral.

ALTER TABLE world_user_activity
  ADD COLUMN total_steps INTEGER NOT NULL DEFAULT 0
  CHECK (total_steps >= 0);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:9e315a33af7a3eee0c3881370c77cf1121a22acdc77f92a8bece490dbe3012ec',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
