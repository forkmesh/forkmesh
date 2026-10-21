CREATE TABLE IF NOT EXISTS custom_reactions (
    scope TEXT NOT NULL,
    slug TEXT NOT NULL,
    label TEXT NOT NULL,
    kind TEXT NOT NULL,
    asset TEXT NOT NULL,
    digest TEXT NOT NULL,
    anim_kind TEXT NOT NULL DEFAULT '',
    anim_asset TEXT NOT NULL DEFAULT '',
    anim_digest TEXT NOT NULL DEFAULT '',
    position INTEGER NOT NULL DEFAULT 0,
    enabled INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    data TEXT NOT NULL,
    PRIMARY KEY (scope, slug)
);
CREATE INDEX IF NOT EXISTS idx_custom_reactions_digest ON custom_reactions(digest);
CREATE INDEX IF NOT EXISTS idx_custom_reactions_anim_digest ON custom_reactions(anim_digest);
CREATE TABLE IF NOT EXISTS comment_tips (
    reference TEXT PRIMARY KEY,
    repo_bi TEXT NOT NULL,
    subject TEXT NOT NULL,
    comment TEXT NOT NULL,
    tipper_bi TEXT NOT NULL,
    recipient_bi TEXT NOT NULL,
    token TEXT NOT NULL,
    mint TEXT NOT NULL DEFAULT '',
    amount TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    signature TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    checked_at INTEGER NOT NULL DEFAULT 0,
    confirmed_at INTEGER NOT NULL DEFAULT 0,
    checks INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_comment_tips_record ON comment_tips(repo_bi, subject, status);
CREATE INDEX IF NOT EXISTS idx_comment_tips_tipper ON comment_tips(tipper_bi, created_at);
CREATE INDEX IF NOT EXISTS idx_comment_tips_recipient ON comment_tips(recipient_bi);
CREATE INDEX IF NOT EXISTS idx_comment_tips_pending ON comment_tips(status, checked_at);
CREATE UNIQUE INDEX IF NOT EXISTS idx_comment_tips_signature ON comment_tips(signature) WHERE signature<>'';
CREATE TABLE IF NOT EXISTS comment_extras_rate (
    k TEXT PRIMARY KEY,
    window_start INTEGER NOT NULL,
    count INTEGER NOT NULL
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:2145ff5525d296868c8e3913e69b80cafcbe43131fdd79f64127a419deab06b3',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
