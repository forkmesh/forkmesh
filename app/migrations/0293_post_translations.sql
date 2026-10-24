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
  'schema_fingerprint:5acceac3d4386ea85e51e3ea37b79eab8aa4b347be6c104682e159bbb3346eb3',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
