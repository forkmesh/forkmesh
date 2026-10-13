CREATE TABLE IF NOT EXISTS marketing_email_campaigns (
        campaign_id TEXT PRIMARY KEY,
        status TEXT NOT NULL DEFAULT 'draft'
            CHECK (status IN ('draft','sending','sent','cancelled')),
        data TEXT NOT NULL,
        created_by TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        started_at INTEGER NOT NULL DEFAULT 0,
        finished_at INTEGER NOT NULL DEFAULT 0,
        cursor_rowid INTEGER NOT NULL DEFAULT 0,
        sent INTEGER NOT NULL DEFAULT 0,
        failed INTEGER NOT NULL DEFAULT 0,
        skipped INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS idx_marketing_email_campaigns_status
    ON marketing_email_campaigns(status, started_at);
CREATE TABLE IF NOT EXISTS marketing_email_sends (
        campaign_id TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        sent_at INTEGER NOT NULL,
        PRIMARY KEY (campaign_id, account_bi));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:a8bb03153d17c76526ffef867fa71126a8fb8478a667bc4eb2e4c5f9d8741f39',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
