-- In-app bug reports. The reporter's prose and any contact string they typed
-- are user copy and stay inside the encrypted `data` blob; the remaining
-- columns are bounded operational metadata the operator console lists and
-- filters on without decrypting. reporter_bi is a blind index of the account,
-- or of the transient client address for a report filed signed out, so the
-- hourly submission quota can count one reporter without the row retaining an
-- address.
CREATE TABLE IF NOT EXISTS bug_reports (
    report_id TEXT PRIMARY KEY CHECK (length(report_id) = 32),
    reporter_bi TEXT NOT NULL DEFAULT '',
    account TEXT NOT NULL DEFAULT '',
    source TEXT NOT NULL DEFAULT 'unknown',
    platform TEXT NOT NULL DEFAULT '',
    app_version TEXT NOT NULL DEFAULT '',
    severity TEXT NOT NULL DEFAULT 'normal'
        CHECK (severity IN ('low','normal','high','crash')),
    status TEXT NOT NULL DEFAULT 'new'
        CHECK (status IN ('new','triaged','closed')),
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at > 0)
);

CREATE INDEX IF NOT EXISTS idx_bug_reports_created
    ON bug_reports(created_at DESC);

CREATE INDEX IF NOT EXISTS idx_bug_reports_reporter
    ON bug_reports(reporter_bi, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_bug_reports_status
    ON bug_reports(status, created_at DESC);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:8ca6018a61314587ee81b58b935c65cebe5174ea5ece9f4e86cd85c799db9774',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
