CREATE TABLE IF NOT EXISTS account_activity_days (account_bi TEXT NOT NULL, day TEXT NOT NULL, device_key TEXT NOT NULL, kind TEXT NOT NULL DEFAULT '', counts TEXT NOT NULL, updated_at INTEGER NOT NULL CHECK (updated_at >= 0), PRIMARY KEY (account_bi, day, device_key)) WITHOUT ROWID;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:ba8c1369b490a37702841863ca3bc5fa40a9cd9f2c1be1471745678c0283713a',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
