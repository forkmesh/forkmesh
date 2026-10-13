ALTER TABLE mail_sends DROP COLUMN dsn;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:33ab0e06e5ba0d0ff7d7ac400261e4b90ebf0d974bcc9f6423a74ef12521894f';
