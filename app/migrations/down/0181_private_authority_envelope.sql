-- Reverse 0181. The added authority columns are dropped; the plaintext
-- device labels, device mirrors and state history that 0181 cleared for
-- private repositories are not restored (the next owner publication rebuilds
-- them under the pre-0181 layout).
ALTER TABLE repo_source_authorities DROP COLUMN data;
ALTER TABLE repo_source_authorities DROP COLUMN is_private;
DELETE FROM schema_meta
 WHERE k = 'schema_fingerprint:793a2aeba9eb2e769ef63443c123e926f831a18845ce10729fa9477e9cdd2f0b';
