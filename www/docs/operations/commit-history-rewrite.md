# Pre-release commit-message rewrite

This procedure rewrites local branch and tag history only after all feature
work, tests, and writers have stopped. It never pushes. The goal is one short,
deterministically generated, name-redacted sentence per commit while preserving
trees, graph topology, author/committer identity policy, and timestamps.

## Safety boundary

The tool defaults to a read-only plan. Execution requires the exact confirmation
phrase `REWRITE-COMMIT-HISTORY`. It rejects Git lock files and scopes itself to
explicit `refs/heads/*` and `refs/tags/*` references.

For every run, owner-only mode-0600 artifacts are placed under
`.git/forkmesh-history-rewrite/<UTC timestamp>/`:

- `pre-rewrite.bundle`, verified by `git bundle verify`;
- `original-commit-messages.jsonl`, containing every old ID, full title,
  description, exact message bytes, and identity headers;
- `old-to-new.json`;
- preflight, ready, and completed scope manifests.

Backup references live below
`refs/forkmesh-history-backup/<UTC timestamp>/`. Neither the artifacts nor
backup refs are worktree content, so they cannot be committed by accident.

Commit signatures, signed annotated tags, old commit links, and scan records
pinned to old IDs become invalid. The tool strips invalid embedded signatures
and records that fact in the manifest.

## Freeze and inspect

1. Stop all agents, editors, hooks, schedulers, CI writers, and background Git
   processes.
2. Record the exact local refs:

   ```bash
   python3 tools/rewrite_commit_messages.py plan
   ```

3. Confirm that the scope contains every intended local branch/tag and no
   remote-tracking or hidden operational ref.
4. Preserve current uncommitted work separately if desired. Commit trees are
   unchanged, so an existing worktree/index remains structurally compatible,
   but an independent worktree backup is prudent.

## Rewrite

```bash
python3 tools/rewrite_commit_messages.py rewrite \
  --confirm REWRITE-COMMIT-HISTORY
```

The deterministic summarizer removes trailers, email addresses, `@handles`,
known author/committer names, and common attribution phrases; it then selects
one bounded sentence. No message or identity data is sent to a model, API, or
network service.

Before moving refs, the tool reconstructs every commit, validates tree IDs,
mapped parent order, exact author/committer headers, sentence length, and name
redaction, then writes the mapping/recovery manifest. Ref changes occur in one
`git update-ref` transaction.

## Validate before coordination

Run the complete Worker, migration, World browser, Qt, iOS, Android, privacy,
security, and performance gates. Also compare:

```bash
git fsck --full
git show-ref --heads --tags
python3 tools/rewrite_commit_messages.py plan
```

Review a representative selection of root commits, merges, old releases, and
recent commits. Do not delete the private artifact or backup refs during the
release stabilization window.

## Restore locally

If validation fails, stop writers again and use the completed artifact:

```bash
python3 tools/rewrite_commit_messages.py restore \
  --artifact-dir .git/forkmesh-history-rewrite/<UTC timestamp> \
  --confirm RESTORE-COMMIT-HISTORY
```

The verified bundle is the independent last-resort recovery source.

## Coordinated mirror update

History publication is a separate, explicitly authorized operation. Announce a
maintenance window and publish the old-to-new mapping without the private
message archive. Make the rewritten refs available atomically, then have each
mirror fetch the exact new tips, verify them, and replace its local branches and
tags. Existing clones should make a backup and re-clone or explicitly reset;
ordinary merges must not reconnect old and new histories. Never perform a blind
`push --mirror`: operational, backup, notes, or private refs may be present.
