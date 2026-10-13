INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:350bb729ba3a9171ee2389d8aeddabfe27573d62e6a611c6b44922256c506516',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
