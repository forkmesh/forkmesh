# Codebase analysis plugins

Five desktop Market packages look at the code of a repository open in
ForkMesh. Two were built-in features turned into plugins; three are new and
share one analysis engine.

| Package | What it shows | Page |
| --- | --- | --- |
| `code-map` | The repository screen's **Code map** button: the codebase at the browsed branch as a zoomable treemap with source drawn in each cell, or a 3D city. | `desktop/src/MainWindowCodeMap.cpp` |
| `size-map` | The repository **Size map** tab: a sunburst of disk use for the repository folder, a picked folder or a drive. | `MainWindow::buildSizeMapTab` |
| `strata` | Architecture and complexity, our take on NDepend: module graph and matrix, cycles, abstractness and instability, rules, technical debt, quality gates. | [strata.md](strata.md) |
| `ember` | File-level hotspots, our take on CodeScene: a circle map coloured by code health, change frequency, refactoring targets, change coupling, knowledge, defects. | [ember.md](ember.md) |
| `synapse` | How files connect, our take on Madge: the import graph as a 3D brain, with circular dependencies in red. | [synapse.md](synapse.md) |

## Code Map and Size Map as plugins

Both features stay in the desktop core; the packages are their on/off switch
and Market listing, the same pattern as [core-plugins.md](core-plugins.md).
`MainWindow::syncPackageSurfaces` hides the **Code map** button and the
**Size map** tab when the package is removed, and leaves the repository on
Code if Size map was the open tab. The footer's disk sparkline opens Size map
only while it is installed and otherwise opens the resource diagnostics.

They are per-device packages (`install.default: false`,
`installedBefore: "2026-10-17"`): projects created before the change keep
both, new projects add them from the Market. The phone apps keep their own
code map screens and do not read these packages.

## The shared engine (`desktop/src/RepoInsight.{h,cpp}`)

Strata, Ember and Synapse read one `forkmesh::insight::Snapshot` per
repository. `Analysis::request(root)` runs the analysis on its own two-thread
pool and shares it: if two pages ask for the same repository while it runs,
both get the same `QFuture`; a finished snapshot is reused for ten minutes
(`Analysis::fresh`), and **Re-analyze** forces a new one. Progress (0-1000)
and the current stage come through the future's progress value and text.

What one analysis does:

1. **List files.** A working tree uses `git ls-files --cached --others
   --exclude-standard`, so ignored files never count. A bare mirror (a
   repository with only `mirrorPath`) lists `HEAD` with `git ls-tree` and
   reads blobs with one `git cat-file --batch` per 1,200 files. A plain
   folder, or a folder inside a repository whose files git ignores, is walked
   directly, skipping hidden, build and vendored folders.
2. **Keep source code.** About 30 languages by extension. Vendored paths
   (`node_modules`, `vendor`, `third_party`, `Pods`, …), minified and
   generated files (`@generated`, `DO NOT EDIT`, protobuf output), binary
   files and files over 2 MB are skipped and counted. `.h` files follow the
   repository's majority (C++ or Objective-C). At most 30,000 source files are
   read; the status line says when a repository is larger.
3. **Measure each file** on a worker pool: comments and strings are blanked
   first, so text inside them never counts. Lines are code, comment or blank
   (a Python docstring line counts as comment). Functions are found from
   their headers before `{` (or `def` / indentation for Python, `def … end`
   for Ruby, Lua and Elixir); each gets its start line, length, cyclomatic
   complexity (1 + `if`, loops, `case`, `catch`, `&&`, `||`, ternaries and the
   language's own branch words), deepest nesting and parameter count. Type
   declarations count types; interfaces, protocols, traits, abstract classes
   and C++ classes with pure virtual functions count as abstract.
4. **Resolve imports** to repository files: relative and aliased JS/TS
   (`.js` specifiers that point at `.ts` sources too), Python absolute,
   relative and `from x import module`, C/C++ includes by directory then by
   path suffix, Go packages through every `go.mod`, Rust `mod` and
   `use crate::/super::/self::`, Java/Kotlin/Scala by path and declared
   package, PHP, Ruby, Dart (`package:` through `pubspec.yaml`), Lua, CSS/SCSS,
   QML and shell `source`. Swift, C#, Kotlin and Java files that use each
   other without an import are linked through the type names they reference
   when a name is declared in at most two files. Imports that do not resolve
   are kept as external packages.
5. **Cycles**: strongly connected components of the import graph (Tarjan,
   iterative, so long chains cannot overflow the stack).
6. **Duplication**: every run of six meaningful lines (imports and
   punctuation-only lines skipped) is hashed; lines inside a run seen twice
   anywhere count as duplicated.
7. **History** (git only): one `git log --no-merges --no-renames --numstat`
   over the last 365 days, at most 8,000 commits. Per file: commits, commits
   in the last 90 days, authors, lines added and deleted, last change, bug
   fix commits (subject words such as fix, bug, crash, regression, revert),
   the main author by lines added, and the share written by former
   contributors (no commit in the last 180 days). Files changed together in
   commits of 2 to 40 files give **change coupling**: degree = shared commits
   / average commits of the two files, kept from 3 shared commits and 25 %.
8. **Code health** (1-10, 10 best) starts at 10 and subtracts for brain
   methods (≥ 70 lines and complexity ≥ 15), complex functions (> 10), very
   complex functions, deep nesting (≥ 4), long functions (> 70 lines), many
   parameters (> 5), large files (> 800 lines of code), many functions in one
   file, duplication over 10 %, dense conditionals, more than 25 imports and
   being in an import cycle. Each penalty adds a readable reason to
   `healthIssues`. Bands: 8 and up healthy, 4 and up problematic, below 4
   unhealthy, as in CodeScene.
9. **Hotspots**: `(commits / most commits)^0.7 × (√lines / √largest)^0.3`,
   scaled so the hottest file is 1.

`buildModuleGraph(snapshot, depth)` groups files by folder prefix. Depth 0
picks the shallowest depth that gives 8 to 60 modules. Afferent and efferent
coupling count files on the other side of the boundary, instability is
Ce / (Ca + Ce), abstractness is abstract types / types, distance is
|A + I - 1|, and `layer` is 0 for modules that use no other module and one
more than the highest module a module uses (cycles share a layer).

`complexityTrend(root, path)` samples up to 16 commits that touched a file
and measures each version with `git show`, for Ember's trend chart.

The engine never writes to the repository and runs only read-only git
commands with `GIT_OPTIONAL_LOCKS=0`, so it never takes the index lock an
agent or the user may need.

On this repository (October 2026) one analysis reads 5,700 source files with
22,000 import edges and a year of history (8,000 commits) in about 9 seconds.

## Host contract

A package that declares the `repository.read` permission gets three
`NativePackageHost` functions (`desktop/src/NativePackageExtension.h`):

- `repositories()`: every local repository with a working tree or mirror on
  this computer, the open one first (`key`, `title`, `path`, `ref`).
- `currentRepository()`: the repository open on the Repos screen, or an empty
  record.
- `openRepositoryFile(repository, path, line)`: opens the file in the
  repository screen's editor at that line when it is the open repository, and
  as a local file tab otherwise.

`NativePackageWidget::repositoryChanged()` is called after the user opens
another repository. `RepoInsightBar` (`desktop/src/RepoInsightBar.{h,cpp}`)
is the header the three pages share: title, repository picker (following
the open repository until the user picks another), **Folder…** for any folder,
**Re-analyze**, a status line and a progress bar.

## Tests

- `forkmesh-repo-insight-tests` (`desktop/tests/test_repo_insight.cpp`):
  language detection, function and complexity measurement for C++, Python,
  TypeScript and Go, import extraction and resolution across languages,
  cycles, history parsing, coupling, health and the module graph. With
  `FORKMESH_INSIGHT_ROOT=<folder>` it analyzes that folder and prints a
  summary instead (timing, edges per language, worst files, unresolved local
  imports).
- Each package has its own suite; see its page.
