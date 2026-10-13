CREATE INDEX IF NOT EXISTS idx_security_incidents_created
  ON security_incidents(created_ms);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:4714e19ba6561716d753ff088486ac70be882ba48f1616fb2682bd69aed075d2',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
