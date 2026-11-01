# Desktop: Git page diffs and pull review

The Git page shows a repository's changes, history and branch or pull
request ranges, with a diff pane beside them. This page covers the page's
layout in the new interface, the change arrows that revert, stage or unstage
one change block, how wide branch and pull diffs render on a worker and page
through the view, and reviewing a pull request in that pane. Index locking
for the Git commands behind it is in [git-locking.md](git-locking.md).

Part of [Desktop](README.md).

## Code

| Part | Files | Tests |
| --- | --- | --- |
| Page and change list | `desktop/src/MainWindowSourceControl.cpp` (`buildSourceControlPanel`, `applyScmChangeBlock()`), `resources/theme/next/52-git.qss` | Git layout check in `desktop/tests/test_window_resize.cpp` |
| Change arrows | `desktop/src/DiffChangeBlock.*` (cutting one change block out of a patch), the split renderer in `desktop/src/MainWindowShared.cpp` (`renderSplitDiffHtml()`, `changeArrowsHtml`) | `desktop/tests/test_diff_change_block.cpp` (`forkmesh-diff-change-block-tests`) |
| Branch and pull diffs | `desktop/src/MainWindowBranches.cpp` (`renderBranchDiffPatchOn()`, `applyBranchDiffChunk()`, `applyBranchDiffRender()`, `setBranchDiffHtml()`); streaming helpers in `desktop/src/MainWindowShared.cpp` (`DiffRenderSink`, `beginDiffStreamIncremental()`, `appendDiffStreamChunk()`, `finishDiffStreamIncremental()`, `buildDiffBlockCache()`) | `checkWideDiffStreamsFromWorker()` in `desktop/tests/test_window_resize.cpp` (`forkmesh-window-tests --diff-stream-only`) |
| Pull review in the pane | `desktop/src/MainWindowPulls.cpp` | `desktop/tests/test_window_resize.cpp` |

## New interface

In the [new interface](ui-redesign.md#pages) the Git page (sidebar Git;
`MainWindow::buildSourceControlPanel` in `MainWindowSourceControl.cpp`,
`52-git.qss`) follows the desktop mockup: a page header with underline tabs
over three columns; Classic keeps the old layout. The page around the panel
is built by `buildRepoCommitsTab`
(`MainWindowRepoDetail.cpp`, not edited); the Next chrome is added from the
panel's own `ScmDesignHook`, one event-loop turn after the panel is polished
or shown, so it only runs once the workspace exists. `ScmNextChrome` (a child
of the panel, object name `scmNextChrome`) holds the Next widgets and
re-arranges both ways on a live design switch: Next to Classic puts every
moved widget back where `buildRepoCommitsTab` placed it, in the same order.
A window that starts in Classic is never touched.

- **Git page in each layout mode** ([two layout
  modes](ui-redesign.md#two-layout-modes-control-tower-and-focus);
  `GitLayout.h`, `gitLayoutCompact()`). Focus is the page described below.
  Control Tower is the compact arrangement: changed files on top of the left
  column and history underneath, no branches column and no page header, with
  the branch picker, Fetch, Pull and rescan in the history header and the
  "▾ CHANGES" caption showing the count. Only the page arrangement changes
  (`arrangePage(false)`); the change rows, the commit box and the history rows
  keep the new design. The commit box sits in the files pane, so Control
  Tower gives the change list a three-row minimum height and raises the files
  pane's 130 px floor to what the panel needs; without them the files/history
  splitter squeezes the list to nothing and cuts off the Commit button in a
  short window. Switching back
  restores the 130 px floor. Switching runs
  `MainWindow::applyGitLayout`, which re-applies the page at once.
- **Header** (`QFrame#gitNextHeader`, inserted above `m_gitWorkspaceStack`
  in the same page layout).
  - "Git" (`#gitNextTitle`) over a muted summary (`#gitNextSummary`): the
    repository name and "N ahead" from the outgoing probe the page already
    runs (`m_scmOutgoingProbePendingCount`, only when it was read for this
    checkout).
  - On the right, the real history-header controls, moved (not copied) out of
    `#gitHistoryHeader`: the branch picker pair (`m_commitsBranchButton` with
    its compare arrow and compare-base button, so its menu, Create new branch
    and switching are unchanged), Fetch (`m_commitsFetchButton`), Pull
    (`m_commitsPullButton`) and the rescan button (`m_scmRefreshButton`).
    Leaderboard stays in the history header.
  - A new Push button with "↑N" calls `pushCurrentRepoUpstream`, the same
    slot as Sync Changes. It is disabled while a push or sync runs, when there
    is no working tree, and when the probe says nothing is ahead.
  - Outline buttons are 28 px on `surface` with a hairline; icons are
    `fgMuted`.
- **Tabs** (`QPushButton#gitNextTab`, `accentFg` underline when checked).
  "Changes (N)" uses the count the Changes header already shows
  (`m_scmCountLabel`). Changes shows the file pane (`m_gitFilesSlot`) and
  History the commit list (`m_gitHistorySlot`) in the left column of
  `gitWorkspaceSplit` (Control Tower stacks them). The tab follows the detail
  stack: opening a commit selects History, the changes or branch-range page
  selects Changes. Clicking Changes leaves an open commit for the changes
  page and runs `refreshSourceControl()`; clicking History runs
  `refreshCommitMarkersIfStale()` and reopens the last commit when it is in
  the loaded list. Both are the refreshes the page runs when it is shown,
  since a hidden pane skips them.
- **Left column** (`GitSidePanel`, `QFrame#gitNextSide`, 240 px, hairline on
  the right), beside `m_gitWorkspaceStack` in a new row layout of the same
  page (the stack keeps its parent). `GitSideRow` paints 28 px rows: foldable
  small-caps headings with a right count, the current branch on
  `accentSubtle` with an `accentFg` check, muted icons, mono `fgSubtle` path
  lines. It shows only data the page already holds, read when the menus are
  rebuilt (`ChildAdded` on their buttons) and on every source-control
  refresh:
  - Branches: the checkable entries of the branch picker's menu; a row
    triggers that entry (`switchToBranch`).
  - Remotes (folded): the entries of the Code toolbar's remotes menu; a row
    triggers it (copies the URL).
  - Worktrees: the rows of the Worktrees table when it was loaded for this
    repository (name and home-shortened path; a row calls
    `switchToWorktree`). Otherwise the toolbar badge's count and an "Open the
    worktree list" link that clicks the Worktrees button.
  - Tags (folded): the toolbar badge's count and an "Open tags and releases"
    link that clicks the Tags button.
- **Change list** (middle column).
  - Groups are `ScmGroupRow`s: "STAGED 2" / "UNSTAGED 5" (11 px medium,
    letter-spaced, `fgSubtle`), with "Unstage all" / "Stage all" text buttons
    (`#scmGroupAction`) that call `scmUnstageAll` / `scmStageAll`. They take
    the place of the "−" and "+" glyphs, which stay in Classic; Open all and
    Discard all stay in both. Range view reads "AGAINST main 3".
  - Rows (`ScmFileRow`, 32 px) are arranged per design from the same
    widgets (`ScmRowParts`): a checkbox (`#scmStageCheck`, staged = ticked;
    it calls `scmStagePath` / `scmUnstagePath`), the status letter in mono
    (A and ? `success`, D `danger`, M `fgMuted`), the muted directory with a
    trailing slash, then the file name. The hover actions (stage, unstage,
    discard, open) and the context menu are unchanged. Classic keeps icon,
    name, directory, status. The checkbox is created only when the row is
    built in Next, so after a live switch it appears with the next refresh.
  - The tree drops the root chevrons in Next (groups still fold by
    double-click or the keyboard) and indents only in folder mode.
  - The "▾ CHANGES" caption and its count are hidden; the tab carries the
    count. Prev, Next, Folders, All repos and the auto-view eye stay.
- **Commit box** (bottom of the middle column, under a hairline
  `#scmNextCommitRule`).
  - "Summary (required)" (`#scmSummaryInput`) and "Description"
    (`#scmDescriptionInput`) mirror the existing message box
    (`m_scmMessage`, hidden in Next): the message is the summary, a blank
    line and the description; auto-fill and Generate write the message box
    and the fields follow. Commit reads `m_scmMessage` as before. Ctrl+Enter
    commits from either field. The sparkle and "⋯" generation buttons sit
    beside the summary.
  - "Commit to <branch>" is the existing Commit button (accent, commit icon;
    the branch is read from the working tree's HEAD file). Its "▾" half
    (`#scmNextCommitMore`) opens a menu that clicks the existing Commit,
    Commit & push and Stage & push buttons, which are hidden in Next. The
    controls row is a plain row in Next and the original `FlowLayout` in
    Classic. The Sync Changes row and its live status line stay.
- **History rows** (`ScmCommitRowDelegate`, 36 px): the short hash
  (mono, `fgSubtle`) and the relative age from the list's own hash and date
  columns are painted on the right, before the ref chips.
- **Diff pane.** Unchanged: it already uses the Next diff renderer (status
  glyph, mono path, +/− counts, hunk rows, line gutters and success/danger
  bands).
- **Left out on purpose.**
  - "N behind", "Pull ↓N" and "Last fetched …": the page never computes how
    far the upstream is ahead or when the last fetch ran; each would need a
    new git call.
  - Stashes: the app reads no stash list.
  - Remote branches under Remotes and the tag list under Tags: only remote
    names and the tag count are in memory.
  - The mockup's per-row "⋯" menu, its discard dialog and "Tick a file to
    stage it": the existing hover buttons, context menu and confirmations
    cover these.
  - Commit author avatars in History: author names stay as text.

## Change arrows in the split diff

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

## Wide branch diffs

The branch/pull review pane must stay responsive on ranges that touch thousands
of files. Fingerprinting, rendering, merge-health scoring and the fold cache
therefore run on a worker, and the result streams into the view.

### How a render runs

- **Skip-rebuild memo.** The render key covers the patch identity (size plus
  hashes of its first and last 64 KB), viewed context, comment anchors, split
  mode, theme, Viewed marks, reopened files and line notes. A request whose key
  matches the markup on screen runs `done` and returns. Any direct write through
  `setBranchDiffHtml()`, an in-place fold or a branch switch clears the key.
- **Small patches render on the GUI thread** (up to 192 K characters with no
  binary sections; the cached repaint `renderBranchDiffPatchNow()` allows
  256 K). Larger patches stream from the worker.
- **Wide patches stream from a worker.** One job fingerprints the patch (or
  reuses fingerprints from the branch read), prunes Viewed marks, renders and
  scores merge health. The renderers pass each ~160 KB of whole files to a
  `DiffRenderSink`; each chunk is queued to the GUI thread and paged in with
  `appendDiffStreamChunk()`, so the first files paint early. The activity log
  shows "Streamed N of M files".
- **The fold cache is built on the worker** per chunk with
  `buildDiffBlockCache()`, which takes the render appearance as an argument
  because the palette cannot be read off the GUI thread.
- **Landing.** `applyBranchDiffRender()` persists the Viewed prune by
  re-reading the stamps (so a tick made during the stream survives), stores the
  health report, lists the files and calls `finishDiffStreamIncremental()`.
- **Cancellation.** A newer render, `setBranchDiffHtml()` or a branch switch
  bumps `m_branchDiffRenderGen` and sets a shared cancel flag checked before
  each chunk. Re-reading an unchanged range keeps the render in flight. A
  Viewed tick during a stream re-renders rather than swapping in place.
- The image-preview memo (`diffImageInlineFor()`) is locked because renders run
  on several threads.

### Scrolling a paged diff

A large diff is planned into bounded pages (about 96 K characters each), and
the view only holds two of them at once. Scrolling past the edge of that window
swaps one page out and the next one in, keeping the rows on screen where they
were (`moveDiffWindowAtEdge()` in `desktop/src/MainWindowShared.cpp`).

The scroll bar thumb is drawn for the whole diff, not for the two pages in the
document, so it does not jump when a page swaps.

- **Thumb geometry** (`diffGlobalScrollGeometry()`): the position is the height
  of the pages above the window plus the view's own scroll value. The range adds
  the height of the pages below the window and the END OF DIFF runway if it is
  not in the document yet. A page swap moves the window and the offset by the
  same amount, so the thumb stays where it was.
- **Page heights**: pages that have been on screen keep their measured height
  (`measureResidentDiffPages()`). Pages not shown yet are estimated from their
  character count at the measured pixels per character. A width change or a
  restyle clears the measurements, and a Viewed fold clears the pages it
  rewrote.
- **Mouse on the bar**: the window filter paints the bar itself and handles
  presses. Dragging the thumb or clicking the track maps the point to a page and
  an offset inside it (`scrollDiffToGlobal()`). A point outside the window shows
  that page (`showDiffPageAtScroll()`) with repaints held until it has streamed
  in. A drag that arrives while a page is still streaming is kept and applied
  once that page is in. The arrow buttons and the wheel work as before.

Test: `checkDiffStreamResponsiveness()` in `desktop/tests/test_window_resize.cpp`
(`forkmesh-window-tests --diff-stream-only`).

## Reviewing a pull request in the Git view

A pull request's "Changes in Git" tab opens its diff in this pane
(`openPullDiffInGitView()`, PR mode: `m_branchDiffPullNumber`). The pane
carries the whole review, in both
[layout modes](ui-redesign.md#two-layout-modes-control-tower-and-focus):

- **Inline comments** work as on the pull screen: click or drag line numbers,
  then Comment now or Add to review. Drafts show inline as "Pending".
- **Review bar** (`buildBranchPullReviewBar()`, `updateBranchPullReviewBar()`
  in `MainWindowPulls.cpp`), between the merge-health strip and the diff, shown
  only in PR mode: "Reviewing PR #N" with the draft count, AI review (same rule
  as the pull header: open, not archived, has a patch), Discard drafts, and
  Review changes / Submit review (N). The bar is named `branchPullReviewBar`:
  its buttons are plain card buttons, not the pull header's rail tiles, so the
  window test's header-style check skips them.
- **Review changes** (`openBranchPullReviewDialog()`) asks for an optional
  summary and Comment, Approve or Request changes (the last two only on an
  open pull). `finishBranchPullReview()` posts the drafts, then the review
  event; a Comment with no drafts and no summary is refused. The composer's
  own Submit review opens the same dialog while it sits on this pane. A
  mirror without write access sends both through the pull inbox
  (`postPullReviewEvent()`, shared with the conversation's Approve and
  Request changes).
- Leaving PR mode (a plain branch, a base change) hides the bar. An AI review
  that finishes re-renders the pane when it still shows that pull.

Test: the "Code on another machine" pull in `test_window_resize.cpp` opens in
the Git view, queues a draft, approves, and checks both events in the pull
store.

## Opening a pull's changes from the list

Hovering a row on the Pull requests list shows a round diff button at the end of
the title (`PullRowDiffDelegate` in `desktop/src/MainWindowPulls.cpp`). A click
opens that pull in the Git range view (`openPullDiffInGitView()`), the same
place the pull's "Changes in Git" tab leads, without first opening the pull's
conversation. The button is painted by the title column's delegate rather than
held as a widget per row, so a long list costs nothing until the pointer is on
a row; the delegate swallows the press, so the click does not also select the
row. While the button is up, the row's background fades in beneath it to clear
the end of the title. It is not drawn when the column is too narrow to fit it.

Test: `testClickPullRowDiffButton()` in `desktop/tests/test_window_resize.cpp`.
