CREATE TABLE IF NOT EXISTS badge_answer_facts (
    repo_bi TEXT NOT NULL,
    number INTEGER NOT NULL CHECK (number > 0),
    dir_oid TEXT NOT NULL DEFAULT '',
    opener_key TEXT NOT NULL DEFAULT '',
    answerer_key TEXT NOT NULL DEFAULT '',
    marked_at INTEGER NOT NULL DEFAULT 0 CHECK (marked_at >= 0),
    updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0),
    PRIMARY KEY (repo_bi, number));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:e450f00ab774ba52e4bf6bceeb6edf3fbeb957859962f869b7457477752aded3',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
