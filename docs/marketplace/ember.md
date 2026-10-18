# Ember package

`marketplace/packages/ember` (Market id `ember`, entry point `builtin:ember`,
rail page **Ember** with the `flame` icon, off by default) shows file-level
hotspots, our take on CodeScene's hotspot map. Every source file of a
repository is a circle, sized by lines of code and nested in translucent
folder circles. Each tab colours the circles by one question: which files are
unhealthy, which change often, which to refactor first, which change together,
who knows them, where bugs get fixed, which language they are in. The numbers
come from the shared analysis engine described in
[codebase-analysis.md](codebase-analysis.md).

## Code map

| Path | Responsibility |
| --- | --- |
| `content/desktop/EmberModel.{h,cpp}` | No widgets. Circle packing (`packSiblings`, `encloseCircles`), the folder/file hierarchy (`buildLayout`), zoom maths (`fitTransform`, `interpolateView`, `readableFocus`), per-tab colouring (`colorize`), filters, refactoring targets, change-coupling index, summary line |
| `content/desktop/EmberMapView.{h,cpp}` | `EmberMapView`: the QPainter map with zoom, pan, selection, labels, coupling links, legend chip and zoom buttons |
| `content/desktop/EmberWidgets.{h,cpp}` | Panel widgets: `RangeSlider` (health range and commit threshold), `HealthRing`, `TrendChart`, `RowList` (ranked targets, coupled files, bands, top hotspots) |
| `content/desktop/EmberFeature.{h,cpp}` | The rail page: `RepoInsightBar` header, tabs, breadcrumb, map, right panel, top-search integration, registration |
| `content/desktop/tests/test_ember.cpp` | `forkmesh-ember-tests`, plus preview mode |

## The page

- **Header.** The shared `RepoInsightBar`: repository picker, **Folder…**,
  **Re-analyze**, status line. Ember adds a summary to the status line, for
  example `301 unhealthy · 953 problematic · top hotspot desktop/src/MainWindow.h`.
- **Tabs.** Hotspots, Refactoring targets, Code health, Change coupling,
  Knowledge, Defects, Languages. With git history the page opens on
  Hotspots; without it, on Code health.
- **Breadcrumb.** Repository / folder / … / file. Each folder segment is a
  link that zooms back out to it; chains deeper than six show the first
  segment, `…` and the last four.
- **Map** (left). Described below.
- **Panel** (right): search by filename, filter by author, a two-handle
  code-health range (1 to 10, in steps of 0.5, the track painted in the band
  colours) and a commit threshold. Below them sits the Refactoring targets
  list (on that tab), the file card when a file is selected, or an overview
  when no file is selected. The overview shows lines of code per health band
  (click a band to filter to it) and the top eight hotspots.
- **File card.** Folder path, file name, **Open file** (ForkMesh's editor),
  **Reveal in folder**, **Copy path**, and three inner tabs:
  - *Metrics*: a health ring with the score and band badge, then Commits
    ("28 commits / 1 year · 9 in the last 90 days"), Size, Main author (with
    share of lines added and author count), Knowledge (share of code by
    former contributors), Defects (fix commits in the window), Modified,
    Complexity (total, and the worst function with a link to its line),
    Functions (count, longest, deepest nesting), Duplication, Imports
    (repository files imported, importers, packages, import cycle) and
    Language. The engine's `healthIssues` are listed underneath.
  - *Complexity trend*: two small charts on a shared time axis, lines of
    code above and total complexity below. They are separate charts because
    the two measures have different scales. `complexityTrend(root, path, 16)`
    runs on a worker the first time the tab opens for a file, and the result
    is cached per path. Hovering shows the commit, author, date, health and
    worst function at that point.
  - *Coupling (n)*: files committed together with this one, with degree bar,
    percentage and shared commits. A click selects that file on the map.

## The map

- **Layout.** Paths become a folder tree. A folder whose only child is a
  folder is merged into one circle named `a/b/c`, so deep Java-style paths do
  not become rings of empty circles. A file's radius is `√max(code lines, 12)`,
  so a one-line file is still about 3.5 units across and clickable. Siblings
  are sorted by size, then name, and packed with the front-chain algorithm of
  Wang et al. (the same as d3-hierarchy `pack`), with 2.5 units of padding
  between siblings. The parent circle is the smallest enclosing circle
  (Welzl's algorithm with d3's fixed LCG shuffle, so the result is
  deterministic) plus the padding and a 3.5 % rim, which leaves room for the
  folder name. Folder health is the line-weighted mean of its files. 20,000
  files pack in about 30 ms in a release build (about 130 ms in debug). The layout and the coupling
  index are built in `QtConcurrent::run`, never on the GUI thread.
- **Drawing.** File circles have a radial gradient lit from the top left, a
  faint drop shadow and a thin darker rim. Folders are translucent discs
  with a thin rim. Dimmed files (filtered out, or not a target on Refactoring
  targets) blend most of the way into the background but stay in place. Labels
  are drawn after all circles: file names are centred when the circle is at
  least 18 px in radius. The font shrinks to fit before the name is cut at the
  end, and a second line shows the tab's metric (health, commits, author,
  fixes, language) when the radius is 38 px or more. Folder names are set
  along the upper rim of folder circles of 70 px or more, letter by letter.
  Names get a dark halo so they read on any colour.
- **Performance.** The still frame is drawn once into a device-pixel-ratio
  aware pixmap; hover, selection, links, badges, glow, legend and buttons
  are overlays on top of it. Subtrees outside the view or under 0.6 px are
  skipped. Files under 2.2 px are drawn in one pass as small squares without
  antialiasing. While a zoom animates or the user drags or scrolls, frames
  skip gradients, shadows and label halos, and draw folder discs without
  antialiasing. About 1,600 nested translucent antialiased discs were most of
  the cost of a frame. 150 ms after the last input the full-quality frame is
  cached again. On 20,000 files a release build draws an animated frame in
  about 16 ms (60 fps) and a full-quality frame in about 50 ms. The test
  checks both.
- **Zoom.** Clicking a folder zooms to the deepest folder under the pointer
  that is at least 14 px in radius and inside the current focus. The move is
  van Wijk and Nuij's smooth zoom, the same path as d3's `interpolateZoom`,
  over 450 ms with a cubic ease. Clicking outside the focused folder zooms
  out one level. Wheel zoom keeps the point under the pointer fixed; dragging
  pans. After free zooming, the breadcrumb follows the deepest folder at the
  view centre that still fills at least 42 % of the view. The buttons at the
  bottom right are zoom in, zoom out and whole repository. Keys: Esc clears
  the selection, then zooms out; Backspace zooms out; `+`, `-`, `0`; Enter
  opens the selected file.
- **Selection.** A click on a file selects it: a dark ring with a light outer
  ring, and a locator ring when the file is under 8 px. Yellow curved links
  then grow out to its coupled files over 420 ms (at most 40 links). Thickness
  and opacity follow the coupling degree, links longer than 70 px carry a %
  label, and the coupled files get a yellow ring. A double click opens the
  file. Choosing a file from search, the coupling list or the targets list
  zooms to the highest folder in which the file is still at least 48 px in
  radius, unless it is already visible at 20 px or more.
- **Legend chip** (top left): an ⓘ icon whose tooltip explains how health and
  hotspots are computed, then the tab's legend: the three bands, a gradient
  scale with end labels, or categorical entries. Hovering a circle shows a
  tooltip with its numbers.
- **Glow.** On Hotspots, the top twelve hotspots that score 0.35 or more
  breathe with a soft orange halo. The 16 ms timer runs only while such a
  file is visible, a zoom animates or links grow, and it stops when the page
  is hidden.

## How each tab colours files

| Tab | Colour | Notes |
| --- | --- | --- |
| Hotspots | Grey → orange → red by `(log(1 + commits) / log(1 + most commits))^1.6`; no commits stays grey | Size already gives area, so a large red circle is a hotspot. The top hotspots glow. |
| Refactoring targets | Health band colour, ranked badges 1–15 | Score = `hotspot × (10 − health)`. Files with a score of 0.05 or less, or with no commits, are not targets. Every other file is dimmed. The panel lists each target with commits, health issues, fix commits, coupling and former-contributor share. |
| Code health | Healthy `#3fb98f`, problematic `#e3b341`, unhealthy `#e5534b` | Bands come from `insight::healthBand`: 8 and up, 4 and up, below 4. |
| Change coupling | Dim amber → yellow by the strongest coupling degree; uncoupled files grey | Links appear for the selected file on every tab. |
| Knowledge | The eight authors owning the most lines get the categorical palette; others grey; files with no commits in the window dark grey | A dashed ring marks files where at least half the lines are by former contributors (no commit in 180 days). Former authors are marked "(former)" in the legend and the author filter. |
| Defects | Pale → deep red by fix commits (log scale); none grey | |
| Languages | Categorical palette for the eight largest languages by lines; others grey | |

The categorical palette is the validated eight-colour dark set from the
dataviz reference (`#3987e5`, `#d95926`, `#199e70`, `#c98500`, `#d55181`,
`#008300`, `#9085e9`, `#e66767`), in a fixed order. It is assigned from the
whole snapshot, so a filter never repaints the remaining files.

Without git history (a plain folder, or history turned off), Hotspots,
Refactoring targets, Change coupling, Knowledge and Defects show a note
saying they need git history, and the author filter and commit threshold are
disabled. The metrics card shows "No git history" for history rows. Code
health and Languages work as usual.

## Filters and search

Filters dim files without moving them: health outside the range, fewer
commits than the threshold, an author who is neither the main author nor one
of the file's top authors, or a path that does not contain the search text
(case-insensitive). Enter in the search box selects the best match: an exact
name first, then a name that starts with the text, then a name that contains
it, then a path that contains it, and the shortest path among equals. `/` or
Ctrl+F focuses the search box.

The window's top search box filters the map the same way while the page is
visible (`setHostSearchQuery`). `searchHits` returns files whose path
contains the query, files whose name matches first, with id `file:<path>`;
activating one selects the file and zooms to it, or remembers it until the
layout is ready.

## Tests and preview

`forkmesh-ember-tests` checks the following:

- **Packing.** 300 random circles never overlap, the radius encloses them all
  and the packing is dense. Two circles and a known smallest enclosing circle
  give the expected results.
- **Layout.** Every file has its node, children lie inside their parents and
  siblings never overlap. Folder chains collapse, totals add up, the minimum
  size holds, `nodeAt` and `chain` work, and the result is deterministic. On
  20,000 synthetic files it is also fast, still free of overlaps and still
  deterministic.
- **Zoom maths.** Interpolation ends exactly on its targets and the readable
  focus works.
- **Colouring.** Every tab's colouring, refactoring-target order and ranks,
  the coupling index, the no-history note and the summary line.
- **Filters.** Health range, commit threshold, author and search, each
  dimming files without removing them.
- **Map view.** Click to zoom into a folder, breadcrumb chain, zoom out, file
  selection, Esc (clear the selection, then zoom out), zoom to a tiny file,
  label drawing, and frame times on 20,000 files.
- **Page.** It renders offscreen, opens on the right tab with and without
  history, and handles the breadcrumb, top search hits and the search filter.

Preview mode: with `FORKMESH_INSIGHT_PREVIEW_ROOT` and
`FORKMESH_INSIGHT_PREVIEW_OUT` set, the test binary analyzes that folder with
the dark theme at 1600×1000. It opens the Code health tab, or the tab named
by `FORKMESH_INSIGHT_PREVIEW_VIEW`, and selects the top non-test refactoring
target, falling back to the least healthy large file. It then zooms so file
names are readable and saves the PNG. `FORKMESH_EMBER_PREVIEW_CARD=1` or `2`
opens the card's trend or coupling tab first.

## On phones

Ember runs on iPhone and Android with the desktop's numbers; the shared parts
(where the code comes from, history, caching, parity tests) are in
[codebase-analysis.md](codebase-analysis.md#on-phones).

| Platform | Model | Page |
| --- | --- | --- |
| iOS | `FMCodeInsight/Ember{Pack,Layout,Model,Panel}.swift` | `ForkMesh/Features/CodeInsight/Ember*.swift` |
| Android | `core/.../codeinsight/Ember*.kt` | `app/.../feature/codeinsight/Ember*.kt` |

The models are ports of `EmberModel` (packing with the same shuffle, layout,
zoom maths, colouring, filters, targets, coupling index, summary) and are
checked against `native/fixtures/insight/ember.json`, written by
`FORKMESH_EMBER_GOLDEN=<fixture> forkmesh-ember-tests`: packing cases, every
layout node, fit and focus, zoom interpolation samples, hit tests, targets,
all seven tabs with and without history, the filter cases, the summary and a
400-file synthetic layout (colours within 1e-4, because the desktop keeps
16-bit channels). Numbers use English grouping and one-decimal
values round half up as Qt does (health 7.25 shows 7.3).

On the phone:

- The tabs are a row of chips; the default tab follows the desktop rule.
- The map is drawn on a canvas: pinch to zoom around the fingers, drag to
  pan, tap a folder to zoom to it, tap a file to select it (with a haptic).
  A double tap fits the whole repository on iPhone and zooms in 2× on
  Android; neither opens the file as the desktop's double-click does
  (**Open file** in the card does). The breadcrumb, legend and the coupling
  links of the selected file are as on the desktop; desktop pixel thresholds
  become points or dp. At most 40 folder labels are drawn (300 file labels on
  iPhone), and on iPhone curved folder names hide while a gesture runs. On
  Android the map is two layered canvases and the glow updates about 20
  times a second, to save battery; the panel sits under the map.
- **Map / List** switches to the files that pass the filters, ranked by the
  tab: hotspot score, target order, lowest health, strongest coupling,
  highest former-contributor share, most fix commits, or language rank then
  lines of code. The list is the screen-reader view of the map.
- Search sits in the navigation bar with suggestions on iPhone and in the
  filter sheet on Android; the filter sheet holds search, author, the two-thumb health range (1 to 10 in steps of 0.5) and the
  commit threshold. Tapping a band in the overview or the filter sheet toggles
  that band's range.
- The file card is a bottom sheet (medium and large) with Metrics,
  Complexity trend (lines of code and complexity on a shared time axis; touch
  shows commit, author, date and health) and Coupling; **Open file** closes the
  sheet and opens the file, **Copy path** and **Share** are next to it.
- Animations respect Reduce Motion and the system animation scale.
