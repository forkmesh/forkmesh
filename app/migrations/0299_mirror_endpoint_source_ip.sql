ALTER TABLE mirror_https_endpoints ADD COLUMN source_ip TEXT NOT NULL DEFAULT '';

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:ba974a9bbe35d8af979fec1e49c70aa805c15d09a63a80db5f856342fcc0e919',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
