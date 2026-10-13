CREATE TABLE IF NOT EXISTS org_office_channels (
    org_bi TEXT NOT NULL,
    room TEXT NOT NULL,
    channel_id TEXT NOT NULL UNIQUE,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (org_bi, room)
);

DROP INDEX IF EXISTS idx_org_office_messages_channel;
DROP INDEX IF EXISTS idx_org_office_messages_expires;
DROP TABLE IF EXISTS org_office_messages;
DROP INDEX IF EXISTS idx_org_office_dms_member_a;
DROP INDEX IF EXISTS idx_org_office_dms_member_b;
DROP TABLE IF EXISTS org_office_dms;
DROP TABLE IF EXISTS org_office_reads;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:18e58402695efbfcd50e3134fdd0c01596b416ce8c6cd86a4dba2afe4ca97749',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
