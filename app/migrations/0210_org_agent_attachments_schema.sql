INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:c98214c4a3dea0015082c2475552687a0673b402d4ab79592ce1546ec080b769',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
