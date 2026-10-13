ALTER TABLE mail_sends ADD COLUMN status TEXT NOT NULL DEFAULT '';
ALTER TABLE mail_sends ADD COLUMN status_at INTEGER NOT NULL DEFAULT 0;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:5b55da66e894c5d0907472e46d05ec99696242bfbf7fbed5928660fc59e38a28',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
