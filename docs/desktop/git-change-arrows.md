# Desktop: change arrows on the Git page

Part of [the desktop docs](README.md). Code: `desktop/src/DiffChangeBlock.*`
(cutting one change block out of a patch), the split renderer in
`desktop/src/MainWindowShared.cpp` (`renderSplitDiffHtml()`, `changeArrowsHtml`)
and `MainWindow::applyScmChangeBlock()` in
`desktop/src/MainWindowSourceControl.cpp`. Test:
`desktop/tests/test_diff_change_block.cpp` (`forkmesh-diff-change-block-tests`).

In the side-by-side view of the Git page's working-tree diff, each change block
(a run of removed and added lines between context lines) carries arrows in the
gutter between the two sides. An arrow copies one side's lines over the other,
as a merge tool does:

| Section | Arrow | Effect | Git command |
| --- | --- | --- | --- |
| Changes | → | the working file gets the left (index) lines back: the change is reverted | `git apply -R` |
| Changes | ← | the index gets the right (working file) lines: the change is staged | `git apply --cached` |
| Staged | → | the index gets the left (HEAD) lines back: the change is unstaged | `git apply --cached -R` |

Staged changes have no ← arrow, because the left side is the commit itself.

- **One block, not the hunk.** `changeBlockPatch()` builds a patch holding only
  the clicked block plus up to three context lines on each side, stopping at
  any other change, so a neighbouring block in the same hunk is left alone.
  `--unidiff-zero` lets a block at the start or end of a file apply without
  the anchoring checks, and `--whitespace=nowarn` keeps `apply.whitespace`
  settings from refusing it.
- **Finding the block again.** The link carries the file path, whether the
  section is staged, and the old and new line numbers where the block starts
  (`scmblock:<path>?s=&o=&n=&d=old|new`). The click looks the block up in the
  patch the pane was rendered from; if the tree has moved on, it refreshes the
  page instead of applying a stale block.
- **Which files get arrows.** Only modified text files in split view. New,
  deleted, renamed and binary files keep the per-file stage, unstage and
  discard buttons. Other diff panes (branches, pull requests, releases) render
  no arrows: the Git page turns them on through `DiffRenderAppearance`.
- A revert has no confirmation, like a merge tool's arrow; it touches one block
  only. Whole-file discards still ask first.
