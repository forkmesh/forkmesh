CREATE TABLE IF NOT EXISTS comment_reactions (
    repo_bi TEXT NOT NULL,
    subject TEXT NOT NULL,
    comment TEXT NOT NULL,
    emoji TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (repo_bi, subject, comment, emoji, account_bi)
);
CREATE INDEX IF NOT EXISTS idx_comment_reactions_account ON comment_reactions(account_bi, created_at);
CREATE TABLE IF NOT EXISTS comment_reaction_rate (
    account_bi TEXT PRIMARY KEY,
    window_start INTEGER NOT NULL,
    count INTEGER NOT NULL
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:32fe1f7b51cfe6b5e1d0faabc48b4ad6e7ac0f7099979b3b0f3e5b924b98c8a4',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
