CREATE TABLE IF NOT EXISTS email_footers (
        footer_id TEXT PRIMARY KEY,
        owner_bi TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        opened_at INTEGER NOT NULL DEFAULT 0,
        last_open_at INTEGER NOT NULL DEFAULT 0,
        open_count INTEGER NOT NULL DEFAULT 0);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:8fb7ed47bd15ac09e076ed0cd85c9a3a1e799ce72edc586c6e8882659cafdcdd',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
