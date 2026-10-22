CREATE TABLE IF NOT EXISTS moderation_roles (
    scope_bi TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'moderator',
    created_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (scope_bi, account_bi));
CREATE INDEX IF NOT EXISTS idx_moderation_roles_account ON moderation_roles(account_bi);
CREATE TABLE IF NOT EXISTS moderation_blocks (
    scope_bi TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    hide_content INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    PRIMARY KEY (scope_bi, account_bi));
CREATE INDEX IF NOT EXISTS idx_moderation_blocks_account ON moderation_blocks(account_bi);
CREATE TABLE IF NOT EXISTS moderation_flags (
    repo_bi TEXT NOT NULL,
    subject TEXT NOT NULL,
    target TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('hide', 'lock')),
    created_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (repo_bi, subject, target, kind));
CREATE TABLE IF NOT EXISTS moderation_queue (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_bi TEXT NOT NULL,
    scope_bi TEXT NOT NULL,
    author_bi TEXT NOT NULL,
    subject TEXT NOT NULL,
    kind TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    claimed_at INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_moderation_queue_scope ON moderation_queue(scope_bi, id);
CREATE INDEX IF NOT EXISTS idx_moderation_queue_author ON moderation_queue(author_bi, repo_bi);
CREATE TABLE IF NOT EXISTS moderation_post_counts (
    account_bi TEXT NOT NULL,
    day INTEGER NOT NULL,
    kind TEXT NOT NULL,
    count INTEGER NOT NULL,
    PRIMARY KEY (account_bi, day, kind));
CREATE TABLE IF NOT EXISTS moderation_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    scope_bi TEXT NOT NULL,
    ts INTEGER NOT NULL,
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_moderation_log_scope ON moderation_log(scope_bi, id);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:01dbf02dd2eaf225fcde111b1508221befaedda2ad1de70f9d646fa15bec5822',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
