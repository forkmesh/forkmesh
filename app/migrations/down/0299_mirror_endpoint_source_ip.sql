ALTER TABLE mirror_https_endpoints DROP COLUMN source_ip;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:ba974a9bbe35d8af979fec1e49c70aa805c15d09a63a80db5f856342fcc0e919';
