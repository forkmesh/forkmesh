# Desktop: wide branch diffs

Part of [the desktop docs](README.md). Code: `desktop/src/MainWindowBranches.cpp`
(`renderBranchDiffPatchOn()`, `applyBranchDiffChunk()`, `applyBranchDiffRender()`,
`setBranchDiffHtml()`), with the streaming helpers in
`desktop/src/MainWindowShared.cpp` (`DiffRenderSink`, `beginDiffStreamIncremental()`,
`appendDiffStreamChunk()`, `finishDiffStreamIncremental()`, `buildDiffBlockCache()`,
`pruneDiffViewedStamps()`). Test: `checkWideDiffStreamsFromWorker()` in
`desktop/tests/test_window_resize.cpp`, run by `forkmesh-window-tests
--diff-stream-only`.

## Why (2026-09-13, merged 2026-09-27)

Opening a branch whose range touched about 5,000 files froze the window for
about two minutes. On the GUI thread, the pane re-read every Viewed mark,
rendered the whole patch to HTML, scored merge health, split the joined markup
again for the fold cache, and built nine widgets per file for the Source
Control list. A range that re-based across an asset directory spent 69 s
decoding, scaling and re-encoding image previews.

Main moved fingerprinting, the render and health scoring to workers on the same
day (the activity log above the pane, `19378536e`). Main also started building
the Source Control rows a batch at a time. This branch adds streaming, the
skip-rebuild memo and the fold cache built on the worker. The merge keeps
main's API (the `done` callback, `renderBranchDiffPatchNow()` for the cached
repaint, the activity log, the stored health report) and main's batched Source
Control rows.

## How a render runs

- **Skip-rebuild memo.** The render key holds the patch identity (its size plus
  hashes of the first and last 64 KB), the viewed context, comment anchors,
  split and theme, the Viewed marks, the reopened files and the line notes.
  A request whose key matches the markup on screen logs "nothing to redraw",
  runs `done` and returns. In one session this path (the pull list re-selecting
  its open row, a cancelled anchor dialog) caused 188 of 658 stalls. A fold
  swapped in place, any other write through `setBranchDiffHtml()` and a branch
  switch (`cancelBranchDiffRender()`) all clear the key.
- **Small patches render on the GUI thread.** At most 192 K characters and no
  binary sections: the render is quick and the view never flashes a
  placeholder. The cached repaint (`renderBranchDiffPatchNow()`) takes this
  path up to 256 K characters, so the pane shows it on the next turn of the
  event loop; a larger cached patch streams from the worker (main's 2026-09-28
  salvage of the cached-repaint freeze).
- **Wide patches stream from a worker.** A single worker job fingerprints the
  patch, or reuses the fingerprints the branch read already computed, and
  prunes the Viewed marks against them. It then renders
  and scores merge health. `renderUnifiedDiffHtml()` / `renderSplitDiffHtml()`
  pass each ~160 KB of whole files to the `DiffRenderSink` instead of keeping
  the full markup. Each chunk is queued to the GUI thread, which pages it into
  the view with `appendDiffStreamChunk()`. The first files paint while the rest
  are still being built. The activity log counts "Streamed N of M files" on one
  line that updates in place. If the view already shows this patch (a fold or
  a note re-rendering it), it stays up until the first chunk arrives; otherwise
  the pane shows "Rendering changes…".
- **The fold cache is built on the worker.** Each chunk is split into per-file
  blocks as it is delivered, and `buildDiffBlockCache()` runs on the worker.
  The GUI thread therefore never re-scans the joined markup when the render
  lands. `buildDiffBlockCache()` takes the render appearance as an argument
  because it cannot read the palette off the GUI thread.
  `splitDiffFileBlocks()` left `MainWindowShared.cpp`'s anonymous namespace for
  this.
- **Landing.** `applyBranchDiffRender()` persists the Viewed prune through
  `refreshDiffViewedForFingerprints()`, which re-reads the stamps, so a tick
  made during the stream is not overwritten. It then stores the worker's health
  report, lists the files, and calls `finishDiffStreamIncremental()` to add the
  end cap and run the stream's finished hooks.
- **Cancellation.** A newer render, `setBranchDiffHtml()` and a branch switch
  bump `m_branchDiffRenderGen` (chunks and landings check it) and set the
  shared cancel flag, which the worker checks before each chunk. A re-read of
  the same range no longer orphans a render in flight: if the patch is
  unchanged, that render is still the right one. A different patch replaces it.
  A Viewed tick while a stream is in flight re-renders instead of swapping in
  place, because the fold cache still describes the previous document.
- The image-preview memo (`diffImageInlineFor()`) is locked, because renders
  now run on several threads at once.

## Merged with main again (2026-10-06)

Main kept its two-stage worker render and added, since the 2026-09-27 merge,
fingerprints computed by the branch read (`renderBranchDiffPatch`'s optional
`fingerprints`), a 256 K cap on the GUI-thread cached repaint, and
`flushBranchDiffAfterRead()` after each read. The streaming render takes all
three: `renderBranchDiffPatchOn()` passes the read's fingerprints to the worker,
`renderBranchDiffPatchNow()` streams a cached patch over the cap, and the error
page and the finished callback flush the queued branch-list read.
