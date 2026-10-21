ALTER TABLE mail_sends ADD COLUMN cause TEXT NOT NULL DEFAULT '';

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:285eaede8b90626859ed5c987c70ff0a689f5f9f61fd5ebe9634b9558ee14933',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
