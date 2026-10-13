-- Branch management requested from the website's Branches page (adhoc #2098).
-- The relay runs no Git, so a delete / update-from-default / create typed on
-- the web is queued here for the repository's own node, which drains it from
-- GET /api/sync, applies it to the checkout every mirror is published from,
-- and propagates the result. `data` holds the encrypted request (operation,
-- branch, the tip the requester saw, and who asked), so no branch name is
-- stored in the clear; repo_bi is the same blind index the other repository
-- inboxes use. The per-repository trigger bounds a queue nobody drains.
CREATE TABLE IF NOT EXISTS branch_op_inbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_bi TEXT NOT NULL,
    data TEXT NOT NULL,
    queued_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_branch_op_inbox_repo
    ON branch_op_inbox(repo_bi, id);

CREATE TRIGGER IF NOT EXISTS trg_branch_op_inbox_repo_bound
BEFORE INSERT ON branch_op_inbox
WHEN (SELECT COUNT(*) FROM branch_op_inbox
       WHERE repo_bi=NEW.repo_bi) >= 64
BEGIN
  SELECT RAISE(ABORT, 'branch_op_inbox_repo_limit');
END;

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:9c8c32bcbc1af75a6fc02c1aafc5f40f244cd6c482fda44f6daece028fd38d1b',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
