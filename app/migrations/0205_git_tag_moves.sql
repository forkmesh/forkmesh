CREATE TABLE IF NOT EXISTS git_tag_moves (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        repo_bi TEXT NOT NULL,
        actor_bi TEXT NOT NULL,
        ref TEXT NOT NULL,
        target TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        used_at INTEGER NOT NULL DEFAULT 0,
        used_from TEXT NOT NULL DEFAULT '');
CREATE INDEX IF NOT EXISTS idx_git_tag_moves_pending
        ON git_tag_moves(repo_bi, actor_bi, used_at, expires_at);
CREATE INDEX IF NOT EXISTS idx_git_tag_moves_created
        ON git_tag_moves(created_at);
