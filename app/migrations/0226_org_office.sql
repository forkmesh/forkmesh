CREATE TABLE IF NOT EXISTS org_office_messages (id INTEGER PRIMARY KEY AUTOINCREMENT, org_bi TEXT NOT NULL, channel TEXT NOT NULL, author_bi TEXT NOT NULL, mentions TEXT NOT NULL DEFAULT '', data TEXT NOT NULL, created_at INTEGER NOT NULL, edited_at INTEGER NOT NULL DEFAULT 0, expires_at INTEGER NOT NULL);

CREATE INDEX IF NOT EXISTS idx_org_office_messages_channel ON org_office_messages(org_bi, channel, id);

CREATE INDEX IF NOT EXISTS idx_org_office_messages_expires ON org_office_messages(expires_at);

CREATE TABLE IF NOT EXISTS org_office_dms (org_bi TEXT NOT NULL, channel TEXT NOT NULL, member_a TEXT NOT NULL, member_b TEXT NOT NULL, data TEXT NOT NULL, last_id INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL, PRIMARY KEY (org_bi, channel));

CREATE INDEX IF NOT EXISTS idx_org_office_dms_member_a ON org_office_dms(org_bi, member_a, updated_at);

CREATE INDEX IF NOT EXISTS idx_org_office_dms_member_b ON org_office_dms(org_bi, member_b, updated_at);

CREATE TABLE IF NOT EXISTS org_office_reads (org_bi TEXT NOT NULL, member_bi TEXT NOT NULL, channel TEXT NOT NULL, last_id INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL, PRIMARY KEY (org_bi, member_bi, channel));

CREATE TABLE IF NOT EXISTS org_office_visits (id INTEGER PRIMARY KEY AUTOINCREMENT, org_bi TEXT NOT NULL, member_bi TEXT NOT NULL, data TEXT NOT NULL, started_at INTEGER NOT NULL, ended_at INTEGER, seen_at INTEGER NOT NULL);

CREATE INDEX IF NOT EXISTS idx_org_office_visits_started ON org_office_visits(org_bi, started_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:8b6c53c28c6058afd79fb9aa15677b7baa746794a6406fe35eed5d9bd818a1ab',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
