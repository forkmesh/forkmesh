ALTER TABLE mail_sends ADD COLUMN dsn TEXT NOT NULL DEFAULT '';

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:33ab0e06e5ba0d0ff7d7ac400261e4b90ebf0d974bcc9f6423a74ef12521894f',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
