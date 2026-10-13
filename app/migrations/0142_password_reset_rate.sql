-- Bound anonymous password-reset email requests without retaining plaintext
-- account identifiers or network addresses. key_bi is a domain-separated
-- blind index for either an account or an edge-observed requester address.

CREATE TABLE IF NOT EXISTS password_reset_rate (
  key_bi TEXT PRIMARY KEY,
  count INTEGER NOT NULL DEFAULT 0,
  window_start_ts INTEGER NOT NULL DEFAULT 0,
  last_sent_ts INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_password_reset_rate_window
  ON password_reset_rate(window_start_ts);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:fe6088a488b813a85591895fabeada9ae8180a0bc3f167247ba598e489fabef6',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
