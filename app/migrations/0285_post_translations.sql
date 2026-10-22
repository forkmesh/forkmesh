CREATE TABLE IF NOT EXISTS post_translations (
    repo_bi TEXT NOT NULL,
    subject TEXT NOT NULL,
    comment TEXT NOT NULL,
    lang TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (repo_bi, subject, comment, lang, account_bi)
);
CREATE INDEX IF NOT EXISTS idx_post_translations_account ON post_translations(account_bi, updated_at);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:7c042c2dee573fb76bf2f28b1376822e0d0e4baaa64821ca850f9cc8f7f8756c',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
