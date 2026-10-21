# Web diff viewer

A pull request's Files changed tab, a branch comparison
(`/owner/repo/compare/base...head`) and a commit page all draw one pane,
`renderRepoPullFilesSection` in `app/public/dashboard/js/06-repo-content.js`.
It follows the phone app's diff review (`native/ios/.../DiffReviewStream.swift`):
the code gets the screen, and the only header that stays put is the file being
read.

## Layout

- **Full width.** While a diff pane is visible, the repository content grid
  drops its 80rem cap and padding (a `:has([data-repo-diff-fullscreen])` rule in
  `shell.html`). Pure CSS, so the pull's other tabs and the rest of the
  repository keep the centred column without any code toggling classes.
- **Changed files on the left.** On screens 1024px and wider the explorer is a
  19rem column flush with the window's left edge, open by default
  (`explorerOpen` in `forkmesh.pull-layout.v1`, remembered when the reader
  hides it). It sticks to the top of the scrolling view and is exactly as tall
  as that view: the scroll-spy writes the view's height into
  `--fm-diff-viewport` on each scroll frame and on resize. Below 1024px it is a
  drawer that slides in over the code with a scrim; picking a file, tapping the
  scrim or Escape closes it, and its state is not saved.
- **The left column holds every control**, so the code side has none: the file
  count and +/- totals, full screen, a settings menu (unified or split, tree or
  list, wrap long lines, collapse or expand all, the keys), hide, the review
  progress bar, the code health indicators, the CI pills, the filter, then the
  file tree.
- **One sticky header: the file.** Each file's own 32px header (path cut from
  the left so the name stays visible, status, its indicators, +/- counts, the
  Viewed button) is `position: sticky; top: 0` inside its file block, so the
  file being read owns the top of the view and the next file's header pushes it
  out. When the explorer is hidden, each header starts with a button that opens
  it.
- **Full screen** puts the pane itself into the browser's full screen
  (`requestFullscreen` on the section, offered only where
  `document.fullscreenEnabled`); the section becomes the scroller.
- **Long lines** scroll sideways inside their file on wide screens and wrap on
  phones (`wrap: auto`). Settings, Wrap long lines, overrides either way. Split
  view always wraps. Inline comment threads stay pinned to the visible width
  while the code scrolls.
- Keys: `j` / `k` next or previous file, `v` toggles Viewed on the file being
  read, alongside the existing `t` and `/`.

## Viewed, with feedback

Marking a file viewed is the review's unit of progress, so it answers:

1. the button pops with a ring and a burst of sparks, a green sweep crosses
   the file's header, the file's row in the tree gets a popping check and an
   animated strike-through, and the progress bar fills (`All N viewed` when
   done, with a glow and a bigger burst);
2. after 280ms the file folds, keeping its header where it was, and the view
   moves to the next file below that is not viewed yet, as the phone app does.

A second click inside that window wins: the fold checks a token first.
Un-marking unfolds the file with a short fade-in and does not move. With
`prefers-reduced-motion` nothing animates and the fold and move happen at once.
The green is the `success` status colour, keyed on `data-viewed`, not on
`aria-pressed`, so the theme rule that keeps green off pressed and selected
controls still holds.

## Code health indicators

The phone's diff health (`native/ios/ForkMeshCore/Sources/FMCode/Collab/DiffHealth.swift`)
is ported line for line as `diffHealthReport` and `diffLineFlags`:

- **Six scores** in the left column, 0 to 100 and green 85+, orange 60+, red
  below: Merge, Complexity, Quality, Security, Network and Practices. Each
  opens a popover with the evidence behind its score and, when added lines are
  flagged for it, a button that steps through them. The web has no Git base or
  conflict check, so Merge is capped at 84, the phone's rule for an unknown Git
  state.
- **Per line**: an added line that trips a rule gets a 3px bar on its left edge
  (red for secrets, TLS bypasses, unsafe calls, open CORS, plain HTTP and
  conflict markers; amber for the rest) and a `⚑ label` tag after the code,
  not selectable, so copying code leaves it out.
- **Per file**: the file header shows one small count per flagged metric
  (clicking steps through that file's lines), and the tree shows a dot.

Flags are computed once per render on the parsed rows (capped at 200,000 added
lines); the section parses the patch once and hands the parse to the file
renderer.

## CI pills

The head commit's workflow runs by outcome (passed, failed, pending, no
verdict), from the same `actions/runs` read as the Checks tab. When the runs
cannot be read (signed out, no executor) nothing is drawn, so an empty row
never reads as a pass. On a pull the pills open the Checks tab.

## Compare and commit pages

The comparison's header is one line and its commit list is a collapsed
`<details>`. A commit page shows a one-line header and the same pane, with
viewed marks kept per commit (`forkmesh.commit-viewed.v1`). The commits tab's
card loses its border and `overflow: hidden` while a commit is open, because
an overflow-hidden ancestor would stop the header from sticking.

Tests: `app/tests/test_dashboard_diff_fullscreen.py`.
