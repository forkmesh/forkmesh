# Diff review on the phone

Both apps show every diff (a branch against its base, one commit, a pull
request's patch, a desktop agent's changes) in one continuous review stream,
like the desktop's diff pane. The Apple Watch has its own compact review of an
agent's changes, read through the phone (see [PARITY.md](PARITY.md), section
4.3.1).

| Piece | iOS | Android |
|---|---|---|
| Rows, viewed marks, find, navigation | `NIC/FMCode/Collab/DiffReview.swift` (`DiffReviewModel`) | `core/.../code/DiffReview.kt` |
| Health scores and line findings | `NIC/FMCode/Collab/DiffHealth.swift` | `core/.../code/DiffHealth.kt` |
| Stream view | `NI/ForkMesh/Features/Code/Collab/DiffReviewStream.swift` | `app/.../feature/code/DiffReviewStream.kt` |
| View settings | `DiffViewSettings` in `DiffViewing.swift` | `DiffViewSettings` in `DiffViewer.kt` |
| Screens | `BranchDiffScreen`, `PatchCommitScreen`, `PatchFilesScreen`, `AgentDiffScreen`, `RepoCommitDetailScreen` | the same plus `CommitDetailScreen` and "Open in diff viewer" on a pull request patch |
| Image previews | `NIC/FMCode/Collab/DiffImages.swift`, `DiffImagePreview.swift` | none |
| Tests | `FMCodeTests/DiffReviewTests.swift`, `CollabBranchDiffTests.swift`, `DiffImagesTests.swift`, `FMAgentsTests/RemoteAgentDiffDeleteTests.swift` | `code/DiffReviewTest.kt`, `agents/ops/AgentOpsTest.kt` |

A commit shows its summary card as the stream's header, with viewed marks
scoped `owner/name/commit/<hash>`.

## Layout

- **Full width.** No card or side padding, narrow gutter, wrapping on by
  default. With wrapping off the whole stream scrolls sideways as one canvas;
  file headers stay pinned at the left.
- **One stream.** Each file has a header (status badge, name, directory or old
  path, `+A −D`, **Viewed** checkbox). The current file's header stays pinned;
  tapping it scrolls to the file's start.
- **File names (iOS).** The whole file name always shows and wraps if needed;
  the directory gets the remaining space and is truncated from its start.
- **Lazy rows.** The model flattens all files into one row list (section,
  header, notice, hunk, line or side-by-side pair, end) with stable integer ids,
  so only visible rows are drawn. Syntax runs are computed per file on first
  display and cached.
- **Cheap scrolling (iOS).** The top row, settle timer and jump flag live in
  `DiffScrollAnchor`, read only by the pinned header overlay
  (`DiffPinnedHeaders`), so scrolling does not re-run the stream's body.
- Every full-width row is sized to the screen width in both wrap modes, so the
  Viewed button (26.5 pt, never shrinks) is never clipped.

## Toolbar

One sideways-scrolling row of small buttons:

1. **`seen/total`** opens the file list (progress, summary, filter past five
   files, viewed toggles, jump to file).
2. **Previous/next change** and **next file to review** (Android only; on iOS,
   marking a file viewed jumps to the next one).
3. **Find**: case-insensitive, capped at 5000 matches, `n/total` count.
4. **Side by side**, **wrap lines**, **syntax highlighting**.
5. **Findings**: text tags and tinted gutter on flagged added lines (red for
   security rules, amber otherwise). Off by default.
6. **Auto-viewed**, and on iOS the **scroll click sound**
   (`diff/scrollSound`), **scroll haptic** (`diff/scrollHaptic`) and **page
   buttons** (`diff/pageButtons`) switches, all on by default.
7. **Text size** 9 to 18 pt (Android buttons; iOS pinch to zoom in half-point
   steps, with VoiceOver actions on the `seen/total` button).
8. **More** (copy patch, copy commit id).

All settings are saved under `diff/*` in app preferences.

On iOS the row has no bar behind it (2026-10-18): each button is its own
glass square with an accent tint when on, like the Inbox squares. The squares
are 30 pt with a 17 pt icon and 4 pt gaps; 36 pt squares with 20 pt icons
pushed most of the row off an iPhone screen. Holding any of them shows its
name in a small popover (`FmHoldName`, `native/ios/ForkMesh/Kit`), and the tap
that ends the hold is dropped so it does not also flip the switch.

The **health strip** shows `+A −D` and six pills (Merge, Complexity, Quality,
Security, Network, Practices), each an icon and score tinted green, orange or
red. Screen readers hear the name; tapping a pill shows the desktop's tooltip
text.

| Score | iOS symbol | Android icon |
| --- | --- | --- |
| Merge | `arrow.triangle.merge` | `call_merge` |
| Complexity | `point.3.connected.trianglepath.dotted` | `account_tree` |
| Quality | `checkmark.seal` | `verified` |
| Security | `lock.shield` | `shield` |
| Network | `network` | `public` |
| Practices | `list.bullet.clipboard` | `rule` |

On the iOS agent diff screens the pills (`DiffHealthPills`, smaller) sit in the
top bar under the branch name (`DiffBranchTitle`) instead of a strip.

## Viewed marks

- Tapping **Viewed** marks the file, folds it and scrolls to the next file.
  Tapping a folded header reopens it without clearing the mark.
- **Auto-viewed** (phone default on, desktop default off): a file counts as
  viewed once its end has scrolled into view, checked 400 ms after scrolling
  stops and only after user scrolling. `autoMarkPassed(from:to:)` also marks
  every file whose end passed the top during a downward scroll. Auto-marked
  files stay open until the diff is reopened.
- **Persistence.** Marks are saved per scope under `diffViewed/<scope>`: the
  desktop's `<owner>/<repo>/branch/<head>`, or `<owner>/<repo>/commit/<hash>`,
  `agent/<branch>`, `patch/<fingerprint>`. Each mark is `<fingerprint> <path>`,
  where the fingerprint is the first 16 hex of the SHA-1 of the file's patch
  section. A changed section loses its mark; a file missing from the patch
  keeps it. The 200 most recent scopes are kept.
- Agent diffs have "Committed on the branch" and "Uncommitted in the worktree"
  sections with separate marks.
- iOS animates the mark (header sweep, checkbox pop, 320 ms before jumping);
  Reduce Motion turns animations off.

## File actions

A "More" button beside **Viewed** (and a long press on the header) opens: Edit
file (branch compares, text files not deleted; Quick edit proposes a pull
request), View file (at the compared branch or commit), Mark as viewed/not
viewed, Copy path, Copy file diff. The host screen supplies Edit and View via
`fileActions`; agent, runner and patch diffs pass none. Deleting a file is
desktop-only.

## Images and SVGs (iOS)

Files ending in `.png`, `.jpg`/`.jpeg`, `.gif`, `.webp`, `.bmp`, `.ico`,
`.heic`, `.tif`/`.tiff` or `.svg` get a Before/After preview row on a
checkerboard with pixel and byte sizes. SVGs keep their text diff below.

- `DiffImages.swift` (`DiffImageKind`, `DiffImageRequest`, `DiffImageLoader`).
  Without a loader there is no preview row. `loadImages(file)` reads each side
  once, on first appearance.
- `AgentDiffScreen` asks the desktop with `agentops.file` (`id`, `path`,
  `change` committed/uncommitted, `side` old/new). Committed: old is the merge
  base, new the branch tip. Uncommitted: old is the worktree `HEAD`, new the
  worktree file. `BranchDiffScreen` reads the mirror's `/raw` at merge base and
  head.
- The desktop answers only for image extensions, refuses absolute paths and
  `.`/`..`, keeps worktree reads inside the worktree after resolving symlinks,
  and sends at most 2 MiB. An older desktop answers `unsupported`.
- SVGs render in a `WKWebView` as a `data:` `<img>` with JavaScript off, a
  content security policy allowing only `data:` images, a non-persistent data
  store and no touches.

## Merge button (iOS)

On agent diff screens **Merge & clean up** is a 42 x 30 pt green glass button
(`MergeGlass` in `MergeCleanupSheet.swift`), a gradient over material before
iOS 26 and solid green with Reduce Transparency.

## Sounds and ratchet

The stream plays shared interface cues (see [NAVIGATION.md](NAVIGATION.md)):
`toggle-on`/`toggle-off` for marks and switches, `success` for the last file,
`navigate` for jumps, `open`/`close` for sheets and folds, `warn` for a failed
find, `click` otherwise.

While scrolling, both apps play a freewheel ratchet: one click per code line.

- A row clicks when its top edge crosses the bottom of the pinned file header,
  in either direction. iOS: `DiffRatchetTooth` (`onGeometryChange` reporting
  only above/below the divider) and `DiffLineRatchet`. Android: `dividerRow`
  over `LazyListLayoutInfo`.
- Code rows and `@@` hunk rows are teeth; other rows are silent. Context clicks
  are soft, changed lines crisp, flagged lines add a low catch.
- Jumps, toolbar actions, folds, marks and pinned-header height changes mute
  the ratchet for 350 ms.
- `RatchetGate` strikes on the first row, then batches per 60 Hz frame
  (`kRatchetBatchSeconds`, Android `RatchetBurst.BATCH_MS`), up to 8 teeth per
  burst.
- Sounds are synthesised at launch in two seed variants. iOS keeps one prepared
  `AVAudioPlayer` per clip; Android one `SoundPool` of six streams. Scrolling
  creates no players or buffers.
- Haptics: `FmHaptics.ratchet` (iOS impacts, Android tick primitives), following
  the Haptics switch and Reduce Motion.

| Piece | iOS | Android |
|---|---|---|
| Synth, burst, gate | `RatchetBurst`, `renderRatchetCatch`, `RatchetGate` in `NIC/FMAgentWatch/InterfaceCues.swift` | `RatchetBurst`, `RatchetGate` in `core/.../ui/SoundCues.kt` |
| Players | `FmCuePlayer.ratchet` in `Theme/FmSoundCues.swift` | `FmSoundCues.ratchet` in `ui/kit/FmSoundCues.kt` |
| Haptics | `FmHaptics.ratchet` in `Theme/FmHaptics.swift` | `FmHaptics.ratchet` in `ui/theme/ForkMeshTheme.kt` |
| Tests | `InterfaceCueTests`, `DiffReviewTests` | `SoundCuesTest`, `DiffReviewTest` |

## Turning pages by voice (iOS)

While a review stream is on screen the Genie understands "next page",
"previous page", "next file" and "previous file" and common variants (see
[GENIE.md](GENIE.md)).

- **Next page** scrolls the lowest visible row (even if cut off) to the top
  under the pinned header; if that row is already at the top, the next row
  goes there. Rows paged past are marked viewed as when scrolling.
- **Previous page** scrolls the top row to the bottom.
- **Next/previous file** jumps to that file's header and speaks its name.
- At either end the Genie says so and nothing moves.
- `DiffRatchetTooth` also reports whether a row's top is above the viewport
  bottom (`DiffRowEdges`); `DiffPageMarks` tracks visible rows.
  `DiffReviewModel.pageDown(top:bottom:)`, `pageUp(top:)`, `nextFile(from:)`,
  `previousFile(from:)` pick the target. The stream registers with
  `GeniePageTurner` via `.genieTurnsPages` while visible; the most recently
  shown stream wins.
- Tests: `DiffReviewTests.testPagesAndFilesTurnByVoice`, `GeniePageTests`.

## Page buttons (iOS)

Two 34 pt glass circles (up and down chevrons, 44 pt tap areas) at the diff's
bottom right call `pageDown` and `pageUp` like the voice commands. At either
end they give a warning haptic. Hidden when the diff has no files; toggled by
`DiffViewSettings.pageButtons` (`diff/pageButtons`).

## Line marks

Every added line a health rule flags shows the icon of the pill it counts
against, at the top of its gutter: red when severe (secret, TLS bypass, unsafe
call, permissive setting, plain HTTP, conflict marker), otherwise amber.

| Finding | Pill |
|---|---|
| secret, TLS bypass, unsafe call, permissive | Security |
| plain HTTP, network call | Network |
| TODO, debug, broad catch, long line | Quality |
| deep nesting | Complexity |
| conflict marker, trailing space | Practices |

`DiffLineFlag.metric` maps a flag to its pill; `DiffLineMark.of` orders marks
severe first.

## Health scores

`DiffHealth` ports `diffHealthReport` from `desktop/src/MainWindowShared.cpp`
rule for rule (classifiers, regexes, penalties, evidence, caveats, and the
secret patterns and `forkmesh-secret-scan:ignore-line` marker from
`RepoSecurity.cpp`). Differences:

- The phone scans the parsed added lines rather than the patch text.
- The ignore marker applies only to its own line.
- The phone has no git divergence or conflict check, so Merge is capped at 84
  with an explanation (a merged agent branch skips the cap).

## Comments on a branch

The branch diff takes inline comments without a pull request, as on the
desktop.

- Tap a line number (or +/- column) to select a line; tap a second line on the
  same side to extend to a range; tap again to clear. `DiffLineSelection` is
  shared with pull review. Only the gutter takes taps.
- A bottom sheet (iOS) or inline panel (Android) sends the comment. Threads
  show under their lines with Reply and Resolve/Reopen; file-level threads are
  shown but cannot be started on the phone. Commenting requires a signed-in
  device and a branch head different from its base.
- One thread card serves pull and branch threads (`ReviewThreadActing`,
  `ReviewThreadActions`).
- Threads are read from `.forkmesh/branch-reviews/<key>` on `forkmesh/pulls`.
  New events are signed with the `branch-comment` v2 string and posted as
  `{"branch", "event"}` to the pull inbox. A sent comment shows as pending until
  a node writes it into the ledger. See
  `www/docs/design/pull-review-threads.md`.
