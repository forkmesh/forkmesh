ALTER TABLE organization_tasks
       ADD COLUMN progress INTEGER NOT NULL DEFAULT 0
       CHECK (progress BETWEEN 0 AND 100);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:66ad83158ac88ac5ed5b8871fd1a7535fed14cc6fafe85bbdec1c2a67a0a8e62',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
