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
