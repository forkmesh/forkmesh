# Pre-release commit-message rewrite

`tools/rewrite_commit_messages.py` rewrites local branch and tag history to one
short, deterministically generated, name-redacted sentence per commit while
preserving trees, graph topology, identity headers, and timestamps. It never
pushes.

## Safety boundary

- The default command, `plan`, is read-only. `rewrite` requires
  `--confirm REWRITE-COMMIT-HISTORY`, refuses to run while Git lock files
  exist, and touches only explicit `refs/heads/*` and `refs/tags/*`.
- Each run writes mode-0600 artifacts to
  `.git/forkmesh-history-rewrite/<UTC timestamp>/`: a verified
  `pre-rewrite.bundle`, `original-commit-messages.jsonl`, `old-to-new.json`, and
  scope manifests (schema `forkmesh.commit-history-rewrite.v1`). Backup refs
  live under `refs/forkmesh-history-backup/<UTC timestamp>/`.
- The summarizer strips trailers, email addresses, `@handles`, known names, and
  attribution phrases locally; no message data leaves the machine.
- Every rebuilt commit is validated (tree IDs, parent order, identity headers,
  sentence length, redaction) before all refs move in one `git update-ref`
  transaction.
- Commit signatures, signed tags, and links pinned to old IDs become invalid;
  embedded signatures are stripped and recorded.

## Procedure

1. Stop every writer: agents, editors, hooks, schedulers, CI.
2. `python3 tools/rewrite_commit_messages.py plan` and confirm the ref scope.
3. `python3 tools/rewrite_commit_messages.py rewrite --confirm REWRITE-COMMIT-HISTORY`
4. Run the full test gates plus `git fsck --full` and `git show-ref --heads --tags`.
5. On failure, restore:

   ```bash
   python3 tools/rewrite_commit_messages.py restore \
     --artifact-dir .git/forkmesh-history-rewrite/<UTC timestamp> \
     --confirm RESTORE-COMMIT-HISTORY
   ```

Publishing rewritten history to mirrors is a separate, explicitly authorized
operation: announce a window, publish the old-to-new mapping (never the message
archive), and have each mirror fetch and verify the exact new tips. Never use a
blind `push --mirror`; clones must re-clone or reset rather than merge old and
new histories.
