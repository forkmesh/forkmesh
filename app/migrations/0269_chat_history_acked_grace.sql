ALTER TABLE chat_history ADD COLUMN acked_at INTEGER NOT NULL DEFAULT 0;

CREATE INDEX IF NOT EXISTS idx_chat_history_acked
    ON chat_history(acked_at) WHERE acked_at>0;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:cfae01526d880e13fdc3ea0f18605f99244efbbc0ce71269dde05109bec15701',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
