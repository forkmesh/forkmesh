-- Private repository source-device labels belong inside the encrypted catalog
-- boundary. Public repositories keep their authority id readable because the
-- anonymous catalog needs it; private rows are rewritten with blank legacy
-- columns and AES-GCM data on their next authenticated publication.
ALTER TABLE repo_source_authorities
  ADD COLUMN is_private INTEGER NOT NULL DEFAULT 0;
ALTER TABLE repo_source_authorities
  ADD COLUMN data TEXT NOT NULL DEFAULT '';

-- Existing private catalog rows already carry the signed canonical node id in
-- encrypted `repositories.data`. Clear the redundant plaintext immediately;
-- the next owner publication repopulates `data` with the protected authority
-- envelope. Secondary-device telemetry can be recreated from a live device and
-- is discarded rather than retaining a plaintext device correlation record.
UPDATE repo_source_authorities
SET node_id = '', machine_name = '', is_private = 1, data = ''
WHERE repo_bi IN (
  SELECT key_bi FROM repositories WHERE is_private = 1
);
DELETE FROM repo_device_mirrors
WHERE repo_bi IN (
  SELECT key_bi FROM repositories WHERE is_private = 1
);
DELETE FROM repo_state_history
WHERE key_bi IN (
  SELECT key_bi FROM repositories WHERE is_private = 1
);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:793a2aeba9eb2e769ef63443c123e926f831a18845ce10729fa9477e9cdd2f0b',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
