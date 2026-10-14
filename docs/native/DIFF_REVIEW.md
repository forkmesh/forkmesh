# Diff review on the phone

Both apps show every diff (a branch against its base, one commit, a pull
request's patch, and a desktop agent's changes) in one continuous review stream,
like the desktop's diff pane. Before this, a diff opened as a file list, and
each file opened in a pager one at a time. The agent "Changes" screen showed raw
patch text in a card.

The Apple Watch has its own compact review of an agent's changes, read through
the phone: see [PARITY.md section 4.3.1](PARITY.md#431-reviewing-a-diff-on-the-watch).

| Piece | iOS | Android |
|---|---|---|
| Rows, viewed marks, find, navigation | `NIC/FMCode/Collab/DiffReview.swift` (`DiffReviewModel`) | `core/.../code/DiffReview.kt` |
| Health scores and line findings | `NIC/FMCode/Collab/DiffHealth.swift` | `core/.../code/DiffHealth.kt` |
| The stream view | `NI/ForkMesh/Features/Code/Collab/DiffReviewStream.swift` | `app/.../feature/code/DiffReviewStream.kt` |
| View settings (persisted) | `DiffViewSettings` in `DiffViewing.swift` | `DiffViewSettings` in `DiffViewer.kt` |
| Screens using it | `BranchDiffScreen`, `PatchCommitScreen`, `PatchFilesScreen`, `AgentDiffScreen`, `RepoCommitDetailScreen` | the same, with `CommitDetailScreen`, plus "Open in diff viewer" on a pull request's inline patch |
| Image previews | `NIC/FMCode/Collab/DiffImages.swift`, `NI/ForkMesh/Features/Code/Collab/DiffImagePreview.swift` | not yet |
| Tests | `Tests/FMCodeTests/DiffReviewTests.swift`, `CollabBranchDiffTests.swift`, `DiffImagesTests.swift`, `Tests/FMAgentsTests/RemoteAgentDiffDeleteTests.swift` | `core/src/test/.../code/DiffReviewTest.kt`, `agents/ops/AgentOpsTest.kt` |

A commit opened from the Commits tab (or a commit link) shows its summary card
as the stream's header, so a commit reads the same as a branch: the same
stream, viewed marks (scoped `owner/name/commit/<hash>`, shared with the
branch's own commit page), find, health scores and file menu. It replaced a
changed-files card over a 420pt preview of the raw patch.

## Layout

- **Full width.** The code runs edge to edge: no card, no side padding, and a
  narrow gutter. Wrapping is on by default, so a phone never scrolls sideways.
  With wrapping off, the whole stream scrolls sideways as one canvas. The file
  headers stay pinned at the left edge of the screen while it does.
- **One stream.** Every file follows the one before it. Each file has a header
  (status badge, name, directory or old path, `+A −D`, and a **Viewed**
  checkbox). The header of the file being read stays pinned at the top. Tapping
  the pinned header scrolls back to the start of that file.
- **Full file names (iOS).** Since 2026-10-14 (branch
  `2004-mobile-file-navigation-buttons`) a file header always shows the whole
  file name; the user asked for it because the name matters more than the
  folder it sits in. The directory (or old path of a rename) beside it gets
  only the space left over and loses its start first (`…/collab/`), so the
  folder nearest the file stays. A name too long for the row wraps onto a
  second line instead of being cut, and the pinned header grows to fit. Before
  this the name and directory were one line cut in the middle, which could
  hide the end of the name. The file list sheet wraps long names the same way.
- **Scrolling stays cheap (iOS).** The top row lives in `DiffScrollAnchor`, an
  observable object that only the pinned header overlay (`DiffPinnedHeaders`)
  reads. Before 2026-10-02 it was view state on the stream, so every row that
  scrolled past re-ran the whole stream's body, toolbar and `ForEach` over all
  rows. The settle timer and the jump flag moved there too, for the same
  reason.
- **Rows are lazy.** The model flattens all files into one row list: section,
  header, notice, hunk, line or side-by-side pair, and end. Rows have stable
  integer ids, so only the rows on screen are drawn, even for a
  thousand-file branch. Syntax runs are computed per file on first display and
  cached.

## The toolbar

This is one row of small buttons under the navigation bar, scrolling sideways
when it does not fit. From left to right:

1. **`seen/total`** opens the file list: progress, the desktop's summary
   ("2 modified, 1 added · 1 left to review"), a filter past five files, a
   viewed toggle per file, and a tap to jump to that file.
2. **Previous change / next change** jump between hunks. This is the desktop's
   Prev/Next. Android only: iOS dropped them on 2026-10-02 (branch
   `1245-move-branch-name-take`) to give the toolbar room.
3. **Next file to review** jumps to the next file not yet viewed, wrapping
   around to the start. Android only, as above; on iOS, marking a file viewed
   still jumps to the next one.
4. **Find** opens a find bar. It shows a `n/total` count or "No results", and
   has previous and next buttons. Matches are case-insensitive and capped at
   5000, as on the desktop, and use the desktop's match colours.
5. **Side by side**, **wrap lines**, **syntax highlighting**.
6. **Findings.** Tags each added line that a health rule flags, for example
   `⚑ TODO · debug`, and tints its gutter. The pill icons on those lines show
   without it (see "Line marks"). Red means a security rule, amber
   anything else. This is phone-only: the desktop has the scores but no
   per-line tags. Off by default.
7. **Auto-viewed** (the eye). This is the desktop's "Automatically mark files as
   viewed while scrolling".
   - **Scroll click sound** (speaker) and **scroll haptic** (vibration), iOS,
     turn the ratchet's click and its Taptic tap on or off for diff scrolling
     only. Both are on by default and saved as `diff/scrollSound` and
     `diff/scrollHaptic`. The app-wide Sounds and Haptics switches still
     apply on top.
   - **Page buttons** (`unfold_more`), iOS, shows or hides the page up and
     page down buttons (see "Page buttons"). On by default, saved as
     `diff/pageButtons`.
8. **Smaller / larger text**, from 9 to 18 points. Android only. Since
   2026-10-02 (branch `1245-move-branch-name-take`) iOS has no buttons for
   it: pinch the diff to zoom. The size follows the pinch in half-point steps
   from the size it had when the pinch began, with a selection tick at each
   whole point, and the ratchet is muted while the rows reflow. VoiceOver gets
   "Larger diff text" and "Smaller diff text" as actions on the `seen/total`
   button.
9. **⋯** holds the screen's extra actions (copy patch, copy commit id).

Under the toolbar, the **health strip** shows `+A −D` and the desktop's six
pills: Merge, Complexity, Quality, Security, Network and Practices. To keep the
strip short, each pill shows an icon instead of its name, followed by the score,
both tinted green, orange or red:

| Score | iOS symbol | Android icon |
| --- | --- | --- |
| Merge | `arrow.triangle.merge` | `call_merge` |
| Complexity | `point.3.connected.trianglepath.dotted` | `account_tree` |
| Quality | `checkmark.seal` | `verified` |
| Security | `lock.shield` | `shield` |
| Network | `network` | `public` |
| Practices | `list.bullet.clipboard` | `rule` |

Screen readers still hear the name ("Merge health score: 84 out of 100,
orange"). Tapping a pill shows the desktop's tooltip text, which starts with the
name: the score, the evidence, the caveat, and the colour bands.

On the two agent diff screens (iOS `AgentDiffScreen` and the runner agent's
diff in `RunnerAgentScreens.swift`), the pills sit in the top bar under the
branch name instead (`DiffBranchTitle`), and the strip is gone, so the code
starts one row higher. Since 2026-10-02 the title shows the branch alone, not
`base...branch` (VoiceOver still reads the base), raised 8 pt, with the pills
1 pt lower, so the two lines no longer crowd each other. The title line already shows `+A −D`. These pills are a
little smaller: 16 pt tall with 9.5 pt text, against 20 pt and 11 pt in the
strip. If they do not fit beside the back and merge buttons, they scroll
sideways. The pills are one view, `DiffHealthPills`, in both places.

The settings (layout, wrap, syntax, findings, auto-viewed, scroll sound and
haptic, page buttons, text size) are saved
under `diff/*` in the app preferences, so they carry across screens and
launches.

## Viewed marks

- Tapping **Viewed** marks the file, folds it to its header, and scrolls to
  the next file, as on the desktop. Tapping a folded header reopens the file
  without clearing its mark.
- **Auto-viewed** follows the desktop rule: a file counts as viewed once its
  end has scrolled into view. The check runs 400 ms after scrolling stops, and
  only after the reader has scrolled. A programmatic jump does not count.
  - Scrolling past a file also marks it, at once. The model's
    `autoMarkPassed(from:to:)` marks every file whose end row lies between the
    old and the new top row of a downward scroll. Before this, only an end
    still on screen when scrolling stopped counted, so a flick that carried a
    file's end past the top edge left that file unmarked.
  - **Header animation (iOS).** When a file is marked by auto-viewed or by a
    tap, a green band sweeps across its header line while a green wash fades
    out. A file marked by scrolling past it has already left the screen, so its
    pinned header stays on top of the next file's for half a second, sweeps,
    pops its checkbox, then lifts and fades away. Reduce Motion turns this off.
  - On the desktop, an auto-marked file folds. On the phone it stays open.
    Folding content the reader is looking at makes the page jump under their
    thumb. The file folds the next time the diff is opened.
  - The phone default is **on**, because that is how a phone reviewer works
    through a branch. The desktop default is off.
- **Persistence.** Marks are saved per scope under `diffViewed/<scope>`.
  - The scope is the desktop's `<owner>/<repo>/branch/<head>`, or
    `<owner>/<repo>/commit/<hash>`, `agent/<branch>`, or `patch/<fingerprint>`
    for a patch with no other identity.
  - Each mark is `<fingerprint> <path>`. As on the desktop, the fingerprint is
    the first 16 hex characters of the SHA-1 of the file's section of the
    patch.
  - A file whose section changed loses its mark. A file missing from the
    current patch keeps its mark.
  - The 200 most recently used scopes are kept; older ones are dropped.
- An agent diff has two sections, "Committed on the branch" and "Uncommitted in
  the worktree". A path in both sections gets a separate mark in each.
- **The Viewed button (iOS)** is 26.5 pt tall, which is 10% taller than the
  first version. It never shrinks: the directory truncates first, then the
  file name wraps (see "Full file names"). Every
  full-width row (the screen header, sections, file headers, notices) is sized
  to the screen width in both wrap modes. Before this, a wide screen header
  could make the wrapped stream wider than the phone, which clipped the button
  at the right edge.
- **Check animation (iOS).** When a file becomes viewed, by a tap, from the
  file list, or by auto-viewed, the checkbox pops and tilts, and a green ring
  pulses out from the button. The jump to the next file waits 320 ms so the
  animation is seen. Reduce Motion turns the animation off.

## File actions

Each file header has a "⋯" button beside **Viewed** (iOS `fileMenu` in
`DiffReviewStream.swift`, Android `FileHeaderRow` in `DiffReviewStream.kt`).
A long press on the header opens the same menu. Before 2026-10-02 the menu
was reachable only by long press, so most people never found it.

- **Edit file** opens Quick edit on the branch being compared. It shows only
  on a branch compare, and only for text files that the branch did not
  delete. Quick edit proposes the change as a pull request; it does not
  commit to the branch.
- **View file** opens the file at the compared branch, or at the commit on a
  commit's diff.
- **Mark as viewed** / **Mark as not viewed**, **Copy path**, **Copy file
  diff**.

The host screen supplies Edit and View through `fileActions`, since only it
knows the repository and ref. Agent, runner and patch diffs pass none.
Deleting a file is a desktop-only action: the mobile API has no endpoint
that deletes a file from a branch.

## Images and SVGs (iOS)

Added on 2026-10-02: a changed image showed only "Binary file — contents are
not shown.", and an SVG only its XML. Now a file whose name ends in `.png`,
`.jpg`/`.jpeg`, `.gif`, `.webp`, `.bmp`, `.ico`, `.heic`, `.tif`/`.tiff` or
`.svg` gets a preview row under its header: **Before** and **After** side by
side (only After for a new file, only Before for a deleted one), on a
checkerboard so transparency shows, with the pixel size and byte size below.
An SVG keeps its text diff under the preview. The binary notice is dropped when
the preview is there.

- **Model.** `DiffImages.swift` (`DiffImageKind`, `DiffImageRequest`,
  `DiffImageLoader`). A screen passes `images:` to `DiffReviewModel`; without a
  loader there is no preview row, so `PatchFilesScreen` and
  `PatchCommitScreen` are unchanged. `loadImages(file)` reads each side once
  per model, when the row first appears; `imageResults` holds the bytes or the
  reason it failed.
- **Where the bytes come from.** `AgentDiffScreen` asks the desktop with
  `agentops.file` (`id`, `path`, `change` committed/uncommitted, `side`
  old/new). Committed: old is the merge base of the base and the branch (what
  `base...branch` diffs against), new is the branch tip. Uncommitted: old is the
  worktree's `HEAD`, new is the file in the worktree. `BranchDiffScreen` reads
  the mirror's `/raw` at the comparison's merge base and head.
- **Limits.** The desktop answers only for image extensions, refuses absolute
  paths and `.`/`..` components, keeps worktree reads inside the worktree after
  resolving symlinks, and sends at most 2 MiB (base64 fits the 4 MiB control
  frame). Larger images say "too large"; an older desktop answers
  `unsupported` and the row asks to update the desktop.
- **SVG safety.** The SVG is drawn by a `WKWebView` as a `data:` `<img>`, which
  never runs scripts or loads anything outside the file. JavaScript is off, the
  page's content security policy allows only `data:` images, the data store is
  non-persistent and the view takes no touches.

## The merge button (iOS)

On the agent diff screens, **Merge & clean up** in the top bar is a 42 × 30 pt
rounded rectangle (corner radius 10) of green Liquid Glass with a white merge
icon (`MergeGlass` in `MergeCleanupSheet.swift`). The toolbar's own glass
background is hidden for it. Before iOS 26 it is a green gradient over a thin
material with a light rim, and a solid green fill when Reduce Transparency is
on. While the merge runs, the icon shakes as before, with a white glow.

## Sounds (iOS)

The stream plays the shared interface cues (`docs/native/NAVIGATION.md`, 6.5)
instead of one selection tick for everything:

| Action | Cue |
|---|---|
| Mark a file viewed | `toggle-on` |
| Clear a viewed mark | `toggle-off` |
| Mark the last unviewed file (by tap or auto-viewed) | `success` |
| Jump to a change, a match or the next file | `navigate` |
| Open the file list, find bar or a health pill | `open` |
| Close the file list or find bar | `close` |
| Fold or unfold a viewed file | `close` / `open` |
| Toolbar switches (layout, wrap, syntax, findings, auto-viewed) | `toggle-on` / `toggle-off` |
| Find with no match on Return | `warn` |
| Text size and other buttons | `click` |

After the last file is marked, the jump to the next file is silent, so the
`success` chime plays in full.

## Ratchet while scrolling (iOS and Android)

Changed 2026-10-02 (branch `1221-diff-scroll-sound-effect`). The first
version, also from 2026-10-02, played a pitched wood note per line that fell
down the pentatonic scale through each file. Reviewers found it musical
rather than useful, and the iOS app crashed while scrolling with it on. It is
replaced by a bike freewheel: the stream sounds like a ratchet turning,
one pawl click per code line.

- **Where a line clicks.** Changed 2026-10-02 (branch
  `1235-diff-clicks-every-line`). A row clicks at the moment its top edge, where
  its line number sits, crosses the bottom edge of the pinned file header: the
  divider the reader watches lines slide under. Scrolling back clicks when the
  line comes back out below it. Before this, a row clicked when it passed the
  top of the screen, hidden behind the pinned header, so the click came about
  two lines late and never matched a visible line.
  - iOS: every code row and hunk row carries `DiffRatchetTooth`, an
    `onGeometryChange` that reports only whether the row's top is above the
    divider, so nothing runs per frame except that one comparison. The divider
    is the measured height of the pinned header. `DiffLineRatchet` keeps the
    last side per row and gathers the rows that crossed in one run-loop turn.
  - Android: `dividerRow` in `DiffReviewStream.kt` finds the row under the
    pinned sticky header's bottom edge in `LazyListLayoutInfo` (38 dp when no
    header is pinned). Every row between the old and the new divider row
    clicks.
- **Every line.** Each code row (a unified line or a side-by-side pair) and
  each `@@` hunk row is one tooth, in either direction. Section, file header,
  notice and end rows are silent. A context or hunk row clicks softly and
  darkly; an added or removed line clicks crisp and metallic; a line with a
  health finding (the same rules as the line marks below) also plays a heavier
  *catch*, a low thunk under the click.
- **Not while the layout moves.** A jump (next change, next file, a find
  match, a tap in the file list), a toolbar button, a fold, a viewed mark and a
  change in the pinned header's height mute the ratchet for 350 ms, so rows
  moving under a reflow do not click.
- **Haptics.** Each strike also taps the Taptic Engine (iOS
  `FmHaptics.ratchet`: a light impact at 0.5 intensity for soft, 0.85 for crisp,
  plus a rigid impact for a catch) or the vibrator (Android
  `FmHaptics.ratchet`: `SegmentFrequentTick`, `SegmentTick`, `ContextClick`).
  They follow the Haptics switch and Reduce Motion, independently of Sounds.
- **Counting.** `scrollPass(from:to:)` (Android) and `scrollPass(crossing:)`
  (iOS) return `DiffScrollPass`: the rows crossed, how many of them changed,
  and how many carry a finding.
- **Pacing.** `RatchetGate` strikes at once on the first row, then gathers rows
  for one 60 Hz frame (`kRatchetBatchSeconds`, 1/60 s; Android
  `RatchetBurst.BATCH_MS`, 16 ms; it was 40 ms) and strikes once per window
  while rows keep coming. A strike is a burst of as many teeth as rows crossed,
  capped at 8 (was 6), spread evenly over the window, so each click lands
  within a frame of its line and a fast flick becomes the freewheel's buzz.
  The burst is crisp when any row in the window changed, and carries the catch
  when any row has a finding.
- **The sound.** Synthesised in code, not from the instrument voices. Changed
  2026-10-02 (branch `1245-move-branch-name-take`) on iOS: the first tick, a
  0.25 ms white-noise snap over 2.1–7.3 kHz partials ringing about 1 ms, was
  thin and harsh. It is now a rounder detent: a softened (low-passed) 0.4 ms
  snap over two or three inharmonic partials (1.18 and 2.71 kHz soft; 1.64,
  3.29 and 5.15 kHz crisp) that ring about 2 ms, start 12% sharp and settle
  within a millisecond, with a 0.18 ms attack and a fade over the last quarter
  of the 8 ms tooth so it never ends on a step. Peaks are about a third lower.
  Android keeps the first tick. Each tooth is detuned up to 3% and varies its
  level by up to 10%, from a seed, so a run never sounds like one sample
  repeated. The catch is a 380 Hz thunk
  with an 1140 Hz overtone, 60 ms, under a crisp click.
- **Playback.** Every burst (two pawls, one to eight teeth) and the catch are
  rendered at launch in two seed variants. iOS keeps one prepared
  `AVAudioPlayer` per clip and restarts it; Android loads them into one
  `SoundPool` of six streams. Scrolling creates no players and no buffers. The
  first version built and started a new `AVAudioPlayer` from a freshly mixed
  WAV every 40 ms on the main thread, which is the suspected crash. It was not
  reproduced on a device, so the cause is not confirmed. Ratchet strikes are
  not interface cues: they skip the cue throttle and batch and cannot swallow a
  click or an error. They follow the Sounds switch and, on iOS, the toolbar's
  scroll click sound switch; the taps follow Haptics and the scroll haptic
  switch.

| Piece | iOS | Android |
|---|---|---|
| Synth, burst, gate | `RatchetBurst`, `renderRatchetCatch`, `RatchetGate` in `NIC/FMAgentWatch/InterfaceCues.swift` | `RatchetBurst`, `RatchetGate` in `core/.../ui/SoundCues.kt` |
| Players | `FmCuePlayer.ratchet` in `Theme/FmSoundCues.swift` | `FmSoundCues.ratchet` in `ui/kit/FmSoundCues.kt` |
| Divider crossing | `DiffRatchetTooth`, `DiffLineRatchet` in `Features/Code/Collab/DiffReviewStream.swift` | `dividerRow` in `feature/code/DiffReviewStream.kt` |
| Haptics | `FmHaptics.ratchet` in `Theme/FmHaptics.swift` | `FmHaptics.ratchet` in `ui/theme/ForkMeshTheme.kt` |
| Tests | `InterfaceCueTests`, `DiffReviewTests.testScrollCountsCodeLinesTheChangedOnesAndTheFlaggedOnes` | `SoundCuesTest`, `DiffReviewTest` |

## Turning pages by voice (iOS)

Added 2026-10-07 (branch `1781-add-ability-say-next`). The user asked to say
"next page" to the Genie while reviewing a diff on the phone and have the
stream page on. While a review stream is on screen the Genie understands
"next page", "previous page", "next file" and "previous file", and the usual
ways of saying them ("page down", "turn the page", "go back a page", "skip to
the next file"). How the Genie hears them is in
[GENIE.md](GENIE.md#spotting-without-a-model).

- **Next page.** The row whose top is lowest on screen, even if it is cut off
  at the bottom, scrolls to the top under the pinned file header, so no line is
  skipped and the cut-off line is read whole. When that row is the one already
  at the top (a tall image preview fills the screen), the next row goes to the
  top instead, so a page always moves. Rows that pass the top this way are
  marked viewed exactly as when scrolling: a page turn is reading, not a jump.
- **Previous page.** The row at the top scrolls to the bottom of the screen.
- **Next and previous file.** Jump to that file's header like a tap in the file
  list, and the Genie says the file's name.
- **At either end** it says "That's the end of the diff", "You're at the top
  of the diff", "That's the last file" or "That's the first file" and does not
  move.
- **Which rows are on screen.** `DiffRatchetTooth` already reports whether a
  row's top is above the pinned header's divider. It now also reports whether
  the top is above the bottom of the viewport (`DiffRowEdges`), in the same
  `onGeometryChange`, so no new per-row observer is added. `DiffPageMarks`
  keeps the rows above the bottom edge and drops a row when it disappears. The
  end row of each file carries the tooth without the ratchet, so the end of the
  last file being on screen means the end of the diff.
- **Where.** `DiffReviewModel.pageDown(top:bottom:)`, `pageUp(top:)`,
  `nextFile(from:)` and `previousFile(from:)` decide the target row; the stream
  scrolls to it (`DiffJump.atBottom` for previous page). The stream registers
  with `GeniePageTurner` through `.genieTurnsPages`, on appear and off on
  disappear, so pushing another screen or changing tab stops paging. When two
  streams are alive the one that appeared last is paged.
- **Tests.** `DiffReviewTests.testPagesAndFilesTurnByVoice` and
  `GeniePageTests`. The scrolling itself was only parsed on Linux; on a phone
  check that a page lands with the cut-off line just under the pinned header,
  in wrapped and unwrapped layouts.

## Page buttons (iOS)

Added 2026-10-14 (branch `2004-mobile-file-navigation-buttons`). The user
asked for two small Liquid Glass buttons at the bottom right of the diff to
step a page down or up without scrolling by hand. Two 34 pt circles of glass
(`shellGlass`, a thin material before iOS 26) with an up and a down chevron
sit about 17 pt in from the bottom-right corner of the diff, each in a 44 pt tap
area. They do exactly what "next page" and "previous page" do by voice (see
"Turning pages by voice"): `DiffReviewModel.pageDown` and `pageUp` pick the
row, so a page lands with the cut-off line under the pinned header, and rows
paged past are marked viewed as when scrolling. At either end the tap gives a
warning haptic and the diff does not move. They are hidden while the diff has
no files. The toolbar's page-buttons toggle turns them off; they are on by
default (`DiffViewSettings.pageButtons`, `diff/pageButtons`). Android has no
page buttons.

## Line marks

Added 2026-10-02. Every added line that a health rule flags shows the icon of
the health pill it counts against, small, at the top of its gutter. These are
the same icons as the pills in the top bar or health strip. The icon is red when
the finding is severe (secret, TLS bypass, unsafe call, permissive setting,
plain HTTP or conflict marker), otherwise amber. In the
unified layout it sits in the old-line-number column, which is empty on an
added line. In the side-by-side layout it sits on a small gutter-coloured
chip over the start of the line number. The marks are always shown. The
**Findings** toolbar switch still adds the text tags and the tinted gutter.

| Finding | Pill |
|---|---|
| secret, TLS bypass, unsafe call, permissive | Security |
| plain HTTP, network call | Network |
| TODO, debug, broad catch, long line | Quality |
| deep nesting | Complexity |
| conflict marker, trailing space | Practices |

Merge has no line rule, so it never marks a line. `DiffLineFlag.metric` maps a
flag to its pill and `DiffLineMark.of` orders the marks, severe first, then in
pill order. The model's `marks(_:_:)` uses the cached flags.

## Health scores

`DiffHealth` ports `diffHealthReport` from `desktop/src/MainWindowShared.cpp`
rule for rule. It uses the same file classifiers, line regexes, penalties,
evidence strings and caveats, plus the secret patterns, placeholder check and
`forkmesh-secret-scan:ignore-line` marker from `RepoSecurity.cpp`.

There are three differences:

- **Line source.** The desktop scans the patch text. The phone scans the added
  lines of the parsed files, which are the same lines.
- **Ignore marker.** The marker suppresses a secret only on its own line. The
  desktop also honours it on the other lines of a backslash-continued logical
  line.
- **Merge score.** A phone has no Git divergence or conflict check. Merge is
  therefore capped at 84 with "this device has no Git base or conflict check
  for the branch", the same cap the desktop applies while its check is pending.
  A merged agent branch skips the cap.

## Comments on a branch (iOS and Android)

The branch diff takes inline comments even when the branch has no pull
request, matching the desktop. Without this, feedback on an agent's branch
first needed a pull request.

- **Selecting:** tap a line number (or the +/− column) to select that line.
  Tap a second line on the same side to extend the selection to a range,
  or tap the selected line again to clear it. This is the same rule as pull
  review: `DiffLineSelection` (iOS `tapped`, Android `next`) is now shared
  by both. Only the gutter takes taps, so code text stays selectable and
  scrolling is unaffected. There is no drag-to-select.
- **Commenting:** a bottom bar (iOS: a sheet; Android: an inline panel)
  sends the comment. Threads appear under their lines, with Reply and
  Resolve/Reopen. File-level threads appear under the file header; the
  phone shows them but cannot start one. Commenting is offered only when
  the diff has a branch head different from its base, and on a signed-in
  device.
- **Thread cards:** one card serves both pull and branch threads, through
  `ReviewThreadActing` (iOS) and `ReviewThreadActions` (Android). On
  Android its title now names the line range ("path:2–4"), and file-level
  threads say "File" on iOS.
- **Storage and signing:** threads are read from
  `.forkmesh/branch-reviews/<key>` on `forkmesh/pulls` with the ordinary
  tree and blob reads. New events are signed with the `branch-comment` v2
  string (the branch name sits where the pull number would) and posted as
  `{"branch", "event"}` to the pull inbox. A sent comment shows as pending
  until a node writes it into the ledger, and the pending copy is dropped
  once it is there. Protocol and server side:
  `www/docs/design/pull-review-threads.md`.
