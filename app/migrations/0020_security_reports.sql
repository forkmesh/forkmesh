-- Private vulnerability reports (adhoc #12).
-- Submissions from /api/security/report are AES-GCM encrypted before storage
-- so only the operator (holding DATA_KEY) can read the plaintext. No IP or
-- other identifying information is stored beyond what the reporter provides in
-- the optional contact field. Bounded by MAX_SECURITY_REPORTS rows.

CREATE TABLE IF NOT EXISTS security_reports (
  id   INTEGER PRIMARY KEY AUTOINCREMENT,
  ts   INTEGER NOT NULL,
  data TEXT NOT NULL         -- AES-GCM encrypted {title, body, component, contact, ts}
);

CREATE INDEX IF NOT EXISTS idx_security_reports_ts ON security_reports(ts);
