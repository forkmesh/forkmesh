# Desktop: wide branch diffs

Part of [the desktop docs](README.md). Code: `desktop/src/MainWindowBranches.cpp`
(`renderBranchDiffPatchOn()`, `applyBranchDiffChunk()`, `applyBranchDiffRender()`,
`setBranchDiffHtml()`) and the streaming helpers in
`desktop/src/MainWindowShared.cpp` (`DiffRenderSink`, `beginDiffStreamIncremental()`,
`appendDiffStreamChunk()`, `finishDiffStreamIncremental()`, `buildDiffBlockCache()`).
Test: `checkWideDiffStreamsFromWorker()` in `desktop/tests/test_window_resize.cpp`
(`forkmesh-window-tests --diff-stream-only`).

The branch/pull review pane must stay responsive on ranges that touch thousands
of files. Fingerprinting, rendering, merge-health scoring and the fold cache
therefore run on a worker, and the result streams into the view.

## How a render runs

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

## Scrolling a paged diff

A large diff is planned into bounded pages (about 96 K characters each), and
the view only holds two of them at once. Scrolling past the edge of that window
swaps one page out and the next one in, keeping the rows on screen where they
were (`moveDiffWindowAtEdge()` in `desktop/src/MainWindowShared.cpp`).

The scroll bar thumb is drawn for the whole diff, not for the two pages in the
document. Before this, each page swap shrank the document under the scroll bar,
so the thumb jumped back up every time the next page loaded.

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
