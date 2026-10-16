DROP TABLE IF EXISTS badge_progress;

ALTER TABLE badge_awards DROP COLUMN tier_at;

ALTER TABLE badge_awards DROP COLUMN tier;

DELETE FROM schema_meta
WHERE k = 'schema_fingerprint:e05a69371b70e57df8b6065a8af3c9bb8df4ebcbab1d573dbd55392cbfefadcb';
