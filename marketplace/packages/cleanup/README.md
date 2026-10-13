# Disk Cleanup

A desktop rail page that shows everything Claude Code, Codex and agent runs
leave on this computer, what each item is still attached to, and deletes what
you confirm. Free, bundled with the ForkMesh desktop, off by default; install
it from the Market.

- `content/desktop/CleanupScanner.{h,cpp}`: finds, sizes and classifies items
  (In use, Attached, Orphaned, Protected) from a context of folders, agent
  sessions, task work items and a `/proc` snapshot. Pure logic, run on a worker.
- `content/desktop/CleanupPlan.{h,cpp}`: turns ticked items and per-item
  choices into a deletion plan, re-checks every path right before deleting it,
  deletes files with `forkmesh::safedelete::removeTree`, and writes
  `cleanup.log` in the ForkMesh logs folder.
- `content/desktop/CleanupFeature.{h,cpp}`: the rail page, filters and the
  confirmation dialog.

Worktree removal, branch retirement and Actions sandbox removal go through the
desktop's host socket `desktop/src/CleanupHostFeed.h`.

Design notes, every category and the safety rules:
[docs/marketplace/cleanup.md](../../../docs/marketplace/cleanup.md).

Tests: `forkmesh-cleanup-tests` (scanner, status classification and deletion
plan against a temporary fake home).
