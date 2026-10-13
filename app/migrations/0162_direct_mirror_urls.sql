-- A mirror's public Git origin is replaceable routing/discovery metadata.
-- Empty preserves the existing Cloudflare Tunnel proxy behavior.
ALTER TABLE mirror_https_endpoints
ADD COLUMN mirror_url TEXT NOT NULL DEFAULT '';

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:945ec97d7a8b89b3b3e97ff818bf7405d6f88964230b38504eb1fbe04cc9c192',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
