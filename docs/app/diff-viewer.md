# Web diff viewer

A pull request's Files changed tab, a branch comparison
(`/owner/repo/compare/base...head`) and a commit page all draw one pane,
`renderRepoPullFilesSection` in `app/public/dashboard/js/06-repo-content.js`.
It follows the phone app's diff review (`native/ios/.../DiffReviewStream.swift`):
the code gets the screen, the controls get two thin rows.

## Layout

- **Full width.** While a diff pane is visible, the repository content grid
  drops its 80rem cap and padding (a `:has([data-repo-diff-fullscreen])` rule in
  `shell.html`). Pure CSS, so the pull's other tabs and the rest of the
  repository keep the centred column without any code toggling classes.
- **Two sticky rows, 30px each** (`.fm-diff-head`, stuck to the top of the
  scrolling view):
  1. *Checks*: the changed-files toggle, the file count and +/- totals, the
     check pills, then icon-only controls (unified or split, viewed count,
     collapse all, settings). The pills scroll sideways on a phone instead of
     wrapping.
  2. *Current file*: the path of the file being read, cut from the left so the
     file name stays visible, its +/- counts, a Viewed checkbox and
     previous/next buttons. The existing scroll-spy (a binary search over the
     file blocks, one per animation frame) updates it, measured from the bottom
     of the header. It replaces the per-file sticky headers; each file keeps a
     thin, non-sticky 28px header as a divider.
- **Changed-files explorer** is collapsed by default (remembered in
  `forkmesh.pull-layout.v1` as `explorerOpen`). Open, it is a sticky left column
  on wide screens and a sticky panel under the header on narrow ones; picking a
  file on a narrow screen closes it.
- **Long lines** scroll sideways inside their file on wide screens and wrap on
  phones (`wrap: auto`). Settings, Wrap long lines, overrides either way. Split
  view always wraps. Inline comment threads stay pinned to the visible width
  while the code scrolls.
- File blocks and diff rows carry `scroll-margin-top` equal to the header, so a
  jump to a file or line lands below it.
- Keys: `j` / `k` next or previous file, alongside the existing `t` and `/`.

## Check pills

- **CI**: the head commit's workflow runs by outcome (passed, failed,
  pending, no verdict), from the same `actions/runs` read as the Checks tab.
  When the runs cannot be read (signed out, no executor) nothing is drawn, so
  an empty row never reads as a pass. On a pull the pills open the Checks tab.
- **Code quality**: a scan of the added lines for the families the phone app's
  diff health strip counts (`DiffHealth.swift`): risky calls, TLS bypasses,
  open CORS and secret-shaped strings (Security), merge-conflict markers,
  leftover debug output, and TODO/FIXME/HACK markers. A pill with hits jumps to
  the first file that has one.

## Compare and commit pages

The comparison's header is one line and its commit list is a collapsed
`<details>`. A commit page shows a one-line header and the same pane, with
viewed marks kept per commit (`forkmesh.commit-viewed.v1`). The commits tab's
card loses its border and `overflow: hidden` while a commit is open, because
an overflow-hidden ancestor would stop the header from sticking.

Tests: `app/tests/test_dashboard_diff_fullscreen.py`.
