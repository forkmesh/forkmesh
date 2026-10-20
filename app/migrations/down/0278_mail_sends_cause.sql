ALTER TABLE mail_sends DROP COLUMN cause;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:285eaede8b90626859ed5c987c70ff0a689f5f9f61fd5ebe9634b9558ee14933';
