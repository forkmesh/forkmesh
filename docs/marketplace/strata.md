# Strata

`marketplace/packages/strata` (Market id `strata`, entry point
`builtin:strata`, rail label **Strata**, icon `layers`) shows the architecture
and complexity of any repository open in ForkMesh, or any folder picked with
**Folder…**. It is our own take on NDepend, built on the shared analysis engine
described in [codebase-analysis.md](codebase-analysis.md): modules are folder
groups, so it works for every language the engine reads.

## Code map

| Path | Responsibility |
| --- | --- |
| `content/desktop/StrataModel.{h,cpp}` | Pure logic: rules, debt, ratings, quality gates, the layered graph layout, the file-level drill-down graph, matrix ordering, the squarified treemap, the HTML report, `buildModel` |
| `content/desktop/StrataViews.{h,cpp}` | QPainter views: `GraphView`, `MatrixView`, `ScatterView` (abstractness × instability), `MetricsView` (treemap), `DashboardView`, `RulesView` |
| `content/desktop/StrataFeature.{h,cpp}` | The rail page: `RepoInsightBar`, view tabs, the details panel, search, export; registers `builtin:strata` |
| `content/desktop/tests/test_strata.cpp` | `forkmesh-strata-tests`, plus the preview mode used for the Market screenshots |

## The page

`RepoInsightBar` sits on top (title, repository picker, **Folder…**,
**Re-analyze**, status). Its action row holds **Modules** (Auto or 1 to 4
folder levels; it rebuilds the module graph from the same snapshot) and
**Export report…**. The status line ends with Strata's summary, for example
`Rating B · 6 d 3 h debt · 2 gates failing`.

Below it the view tabs (blue when active) and, on the right, the view's own
controls and **Details**, which opens or closes the details panel. The panel
shows the selected module (path, rating, lines, files, functions, complexity,
types, health, Ca, Ce, I, A, D, cohesion, commits, interest, worst rules, the
modules it uses and the modules that use it, its files with the most debt) or
the selected file (rating, metrics, health reasons, history, violations, the
files it imports and that import it). Every name in it is a link: modules and
files select, violations open the file at the line.

Keyboard: Esc clears the selection (or leaves a drill-down), `/` and Ctrl+F
open Rules with its filter focused. While the page is visible the window's top
search box dims what does not match in the graph, the scatter and the treemap
and filters the rules; its popup lists matching modules and files, and
choosing one selects it (a file in the graph drills into its module).

### Views

- **Dashboard.** Rating tile (letter, debt ratio, total debt, annual
  interest), metric tiles (lines of code, files, modules, types, functions,
  average and max complexity, comment %, duplicated %, rule violations), the
  quality gates, violations by severity with the rules that cost most, and the
  modules and files with the most debt. Click selects, double-click opens.
- **Dependency graph.** Modules as rounded boxes sized by the square root of
  their lines of code: a header strip coloured by **Colour** (debt rating,
  health, instability or complexity, with a legend), a lines/files caption and
  a mini treemap of the module's files coloured by health. Edges are bezier
  curves with arrowheads, thickness by log(imports), cycle edges red. A click
  dims everything else with an eased fade and highlights what the module uses
  (blue) and what uses it (purple #bc8cff). Double-click drills into the
  module: its files (up to 120, the most connected first) plus the modules
  they use (below) and the modules that use them (above), collapsed to one
  node each; the breadcrumb goes back. Wheel zooms around the pointer, drag
  pans, **Fit** frames the graph.
- **Matrix.** Dependency structure matrix in layer order. A cell is the number
  of file imports: blue when the row uses the column, purple when the column
  uses the row, red when both (a cycle). Cycle groups are outlined on the
  diagonal; layer boundaries are drawn as lines. Cells shrink to fit (9 to 46
  px) and scroll beyond that, with sticky row and rotated column headers.
  Hover crosshairs the row and column; a click lists the file imports behind
  the cell in the details panel.
- **Abstractness × Instability.** Modules as bubbles sized by lines of code
  and coloured by distance from the main sequence, with the zone of pain
  (bottom left, red) and the zone of uselessness (top right, amber).
- **Metrics.** Squarified treemap of files grouped by module; **Size** by
  lines of code, complexity or functions, **Colour** by health, max
  complexity, debt ratio or change frequency, with a gradient legend.
- **Rules.** Category → rule → violations, with a filter box and the selected
  rule's description and how to fix it. Violations load when a rule is
  expanded (500 at most; the report has them all). Double-click opens the file
  at the line.

## How it is computed

### Rules

Each rule has an id, category, default severity, description and fix; each
violation has its own severity, a message, a place and its debt in minutes.
Debt grows with how far over the threshold the code is and is capped per
violation.

| Id | Rule | Fires when | Severity | Debt (minutes) |
| --- | --- | --- | --- | --- |
| ARC01 | Module dependency cycles | each module edge inside a cycle | Critical | 30 + 15 × imports, ≤ 480 |
| ARC02 | File import cycles | each file in an import cycle | High | 15 × imports inside the cycle + 5 × cycle size (≤ 20), ≤ 240 |
| ARC03 | Stable dependencies | a module edge (not in a cycle) where I(target) > I(source) + 0.25 | Medium | 20 + 120 × (gap − 0.25) + 4 × imports, ≤ 240 |
| ARC04 | Zone of pain | module with types, ≥ 150 lines, Ca ≥ 2, A ≤ 0.2, I ≤ 0.3 | Medium | 30 + 150 × (D − 0.5)/0.5 × min(3, √(kLOC)) + 5 × Ca, ≤ 960 |
| ARC05 | Zone of uselessness | module with types, ≥ 50 lines, A ≥ 0.7, I ≥ 0.7 | Low | 20 + 100 × (D − 0.4)/0.6, ≤ 240 |
| DES01 | Hub files | imported by ≥ 10 files and imports ≥ 10 | Medium (High at 25/25) | 30 + 3 × excess, ≤ 480 |
| DES02 | Too many imports | > 25 imports | Low (Medium > 50) | 10 + 2 × excess, ≤ 240 |
| CS01 | Complex functions | cyclomatic complexity > 10 | Medium (High > 20) | 15 + 6 × (cc − 10), ≤ 480 |
| CS02 | Long functions | > 60 lines | Low (Medium > 100) | 10 + 0.6 × (lines − 60), ≤ 480 |
| CS03 | Deep nesting | nesting > 5 | Medium (High > 7) | 20 + 15 × excess, ≤ 240 |
| CS04 | Too many parameters | > 6 | Low (Medium > 9) | 10 + 6 × excess, ≤ 120 |
| CS05 | Brain methods | > 80 lines, cc > 15 and nesting ≥ 4 | High | 60 + 0.5 × extra lines + 5 × extra cc + 15 × extra nesting, ≤ 960 |
| MNT01 | Large files | > 1,000 code lines | Medium (High > 2,000) | 30 + 0.06 × extra lines, ≤ 960 |
| MNT02 | Duplicated code | ≥ 12 duplicated lines | Low (Medium ≥ 100) | 10 + 1.5 per line, ≤ 960 |
| MNT03 | Complex files without comments | not a test, complexity ≥ 40, ≥ 150 lines, comments < 5 % | Low | 15 + 0.25 × (complexity − 40), ≤ 240 |
| MNT04 | Knowledge islands | ≥ 2 authors in the repository, the file is among the most changed (≥ 8 commits and the top quarter) and one author wrote > 90 % of it | Medium | 30 + 4 × extra commits + 0.02 × lines, ≤ 480 |
| MNT05 | Former contributors in hotspots | > 50 % of the lines by people with no commit in 180 days and hotspot ≥ 0.3 | Medium (High > 80 % and > 0.6) | 20 + 200 × hotspot × share, ≤ 480 |

Strata finds module and file cycles itself (Tarjan over the module edges and
over `dependsOn`), so the rules do not depend on the engine's cycle flags.

### Debt, ratio and rating

- Effort to develop the code is NDepend's default: 18 man-days per 1,000 lines
  of code, 8-hour days, so 8.64 minutes per line.
- Debt ratio = total debt ÷ effort. Rating: A ≤ 5 %, B ≤ 10 %, C ≤ 20 %,
  D ≤ 50 %, E above. Files and modules get their own ratio and rating; a
  module's debt is its files' debt plus its module-level violations.
- Debt is shown in days, hours and minutes with 8-hour days.
- Annual interest: each change to a file is assumed to cost 5 % of the file's
  debt in extra work. Interest = debt × 0.05 × changes per year (commits in
  the history window scaled to a year), at most twice the debt per file.
  Module-level debt uses the module's commits. Zero without git history.

### Quality gates

| Gate | Warn | Fail |
| --- | --- | --- |
| Technical debt rating | D | E |
| No critical rule violated | | any critical violation |
| No module dependency cycle | | any cycle |
| No function above complexity 50 | max > 30 | max > 50 |
| Debt in the most-changed files (top 10 % by commits) | ratio ≥ 10 % | ratio ≥ 20 % |

Without history the last gate passes and says so.

### Graph layout

`layeredLayout` is a deterministic Sugiyama-style layout:

1. **Layers.** The engine's `ModuleNode::layer` (0 = foundation) when it is
   consistent with the edges, otherwise longest path over the condensation of
   strongly connected components. Dependents go above their dependencies.
2. **Cycles.** Members of a cycle share their layer, then are spread over
   sub-rows: a greedy feedback arc ordering (Eades–Lin–Smyth, weighted by
   imports) keeps the heavy edges pointing down, so the lightest edges of the
   cycle are the ones drawn upward — the ones to cut.
3. **Width bound.** A row holds at most about √(1.5 × n × row height ÷ box
   width) boxes (at least 4); nodes over the bound are pushed up a row, which
   pushes their dependents up too, so wide graphs fold into a readable shape.
4. **Crossings.** Long edges get dummy nodes; 16 barycentre sweeps (down and
   up, cycle members kept together) keep the ordering with the fewest
   crossings, counted with a Fenwick tree.
5. **Coordinates.** Ten alternating passes place each row at the weighted mean
   of its neighbours by isotonic regression (pool adjacent violators), which
   keeps the order and the gaps; dummy nodes weigh more, so long edges run
   straight.
6. **Routing.** Edges leave the bottom of the source and enter the top of the
   target at ports spread by the neighbour's position and pass through their
   dummy nodes as vertical-tangent beziers. Modules with no edges go in a grid
   below the graph, or beside it when the graph is taller than wide.

The matrix lists modules from the top layer down; members of a cycle are kept
together.

### Performance

The analysis itself is the engine's. Strata builds the module graph, rules,
scores, layout and matrix order in `QtConcurrent::run` (`buildModel`) and hands
the immutable `Model` to the views. The graph caches its static layer in a
device-pixel-ratio-aware pixmap and redraws only the highlight and hover on
top; the highlight animation runs only while it changes and stops when the
page hides. Labels, captions and mini treemaps drop out when boxes get small.
The treemap caches its picture and hit-tests by module first. The rules tree
creates violation rows only when a rule is expanded.

## Report

**Export report…** writes one self-contained HTML file (inline CSS, no
scripts): dashboard numbers, quality gates, every rule with its description,
fix and up to 200 violations, and the module table (rating, lines, files, Ca,
Ce, I, A, D, debt).

## Tests and previews

`forkmesh-strata-tests` covers rule firing on synthetic snapshots (cycles,
zone of pain and uselessness, stable dependencies, complex, long, nested and
brain functions, large and duplicated files, knowledge islands, former
contributors, hub files), debt scaling, debt and rating maths, every gate
outcome, the layout (no overlaps, edges point down, hinted layers, cycles
broken at the lightest edge, wide rows folded, isolated modules), matrix
ordering, the treemap, the drill-down graph, the report sections, every view
offscreen, and the page end to end on a small temporary repository analysed
by the real engine.

With `FORKMESH_INSIGHT_PREVIEW_ROOT` and `FORKMESH_INSIGHT_PREVIEW_OUT` set the
test binary instead renders the page in the dark theme at 1600×1000, selects
the module in a cycle with the most edges and saves a PNG.
`FORKMESH_INSIGHT_PREVIEW_VIEW` picks `dashboard`, `graph` (default),
`matrix`, `scatter`, `metrics`, `rules` or `drill`;
`FORKMESH_INSIGHT_PREVIEW_DEPTH` sets the module depth.
