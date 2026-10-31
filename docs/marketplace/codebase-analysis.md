# Codebase analysis plugins

Five Market packages look at the code of a repository open in ForkMesh. Two
were built-in desktop features turned into plugins; three are new, share one
analysis engine, and run on the desktop, the iPhone and Android with the same
results (see [On phones](#on-phones)).

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

Equal lines in the duplication step are found with a 64-bit FNV-1a hash of
the normalized line's UTF-16 code units followed by a murmur finalizer. It
replaced `qHash`, whose AES-based variant on some CPUs gave the same value for
short repeated lines (`VALUES (?,?,?,?,?,?)` and the same with one more `?`),
so the duplicated-line count depended on the CPU and differed from the phones.

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

## On phones

Strata, Ember and Synapse also run in the iPhone and Android apps (manifest
entry points `ios` and `android`, `install.sync: "account"`, places mobile and
desktop since 1.1.0). There is no server endpoint that serves a repository's
source and history, so each phone analyses the code itself with its own git
engine and a port of the engine above.

### Engine ports

| Platform | Engine | Page logic |
| --- | --- | --- |
| iOS | SwiftPM module `FMCodeInsight` (`native/ios/ForkMeshCore/Sources/FMCodeInsight`, dependency-free) | `Ember*.swift`, `Strata*.swift`, `Synapse*.swift` in the same module |
| Android | package `com.forkmesh.nativeapp.codeinsight` in `native/android/core` | `Ember*.kt`, `Strata*.kt`, `Synapse*.kt` in the same package |

Each is a rule-for-rule port of `RepoInsight.cpp` (and of `EmberModel`,
`StrataModel`, `SynapseLayout`) with the same thresholds, tie-breaks, health
issue texts, progress texts and number formatting (English grouping, ties
rounded half up like Qt), so a phone shows the desktop's numbers. The ports
work on UTF-16 like `QString`. QRegularExpression is PCRE2 without Unicode
properties, so `\w`, `\s` and `\b` are ASCII there: the Swift port carries a
small regex engine over UTF-16 arrays and the Kotlin port rewrites those
classes to ASCII ones, because `NSRegularExpression` and Android's regex are
Unicode-aware. Where the desktop iterates a `QHash` (Go module roots, coupling
ties, module-cycle ties) the ports use a stable order; this never changes a
result except between equal candidates.

`Insight.analyze(source, options, now, progress, isCancelled)` takes an
`InsightSource` (name, head, branch, `entries()`, `read(path, limit)`,
`history(options)`) instead of a folder, and reproduces `analyze()` from the
source filter on. `parseGitLog` reads the desktop's `git log` format,
`applyHistory` takes parsed commits, and `trend(path, versions)` measures the
versions the caller supplies.

### Parity tests

`native/fixtures/insight/` is shared by the desktop and both phones:

- `repo/`: 56 files in about 25 languages, with vendored, generated,
  minified, binary and empty files that must be skipped, two import cycles,
  duplicated code, a brain method and a large file. Every fixture file must
  be committed: the golden writer analyses the folder on disk, so a file git
  ignores (`node_modules/`, `build/`, `__pycache__/`, `.venv/`) ends up in
  `expected.json` but not in other checkouts. The vendored sample is
  `vendor/left-pad/index.js` for that reason.
- `history.log`: fifteen commits in the desktop's `git log --numstat`
  format, with five authors (one former), fix commits, a binary file and a
  path that no longer exists.
- `expected.json`: what the desktop engine produces, written by
  `FORKMESH_INSIGHT_GOLDEN=native/fixtures/insight forkmesh-repo-insight-tests`
  (`writeGolden` in `desktop/tests/test_repo_insight.cpp`: the repo is copied
  to a temporary git repository, analysed with history off, then the log is
  applied with `now = 1792000000`). Lists of paths are sorted and couplings
  compared as a set, because the desktop's coupling order has hash-dependent
  ties.
- `ember.json`, `strata.json`, `synapse.json`: the page models for the same
  snapshot, written by `FORKMESH_EMBER_GOLDEN`, `FORKMESH_STRATA_GOLDEN` and
  `FORKMESH_SYNAPSE_GOLDEN` modes of the packages' desktop tests.

The Swift tests (`Tests/FMCodeInsightTests`) and Kotlin tests
(`core/src/test/.../codeinsight`) assert every field of every file, cycle,
coupling, module node and edge, the 45 path probes, and every page model
value. `FM_INSIGHT_FIXTURE=<dir>` (Swift) and `FORKMESH_INSIGHT_FIXTURE=<dir>`
(Kotlin) point the golden tests at another fixture: run the desktop golden
writer on any folder with a `repo/` and a `history.log` to compare a real tree.
In October 2026 a 3,113-file tree with 404 commits matched on every field.

Synapse's graph, lobes and queries match exactly. Its 3D positions are
compared after 1, 10, 60 and 300 layout iterations: the Swift port keeps Qt's
operation order and `hypot` and is bit-exact against the desktop on Linux; the
Kotlin port is exact for 10 iterations and within 3e-4 after 300, because the
JVM rounds `sin`, `cos` and `log` correctly where glibc is one step off for
about 1% of inputs. On Apple's maths library the Swift tolerance is loose for
the same reason; the desktop's own layout differs between machines too.

### Where the code comes from

The shell (iOS `Features/CodeInsight/CodeInsightScreen.swift` with
`FMCode/Insight/*`; Android `feature/codeinsight/*` with
`core/.../codeinsight/Insight{GitSource,AnalysisCopy,Targets,LineDiff}.kt`)
picks a repository: the route's (`/dashboard/<id>?repo=owner/name`, or the
repository screen's menu), else the last one analysed, else a picker listing
copies on this phone first and the account's repositories after.

- **A copy on this phone** (Local repositories) is read in place: the HEAD
  tree from the object store, preferring the file on disk. Those copies are
  shallow (depth 1), so they have no history and the pages show what needs
  none (Ember opens on Code health).
- **Any other repository** is fetched into an analysis copy in the app's
  cache folder (`Caches/CodeInsight/<owner>/<name>` on iOS,
  `cacheDir/CodeInsight/<owner>/<name>` on Android), never into Local
  repositories, with the endpoints, signing and size limit of Clone to device.
  The first clone goes to a temporary folder and is moved into place.
  **Re-analyze** fetches again and deepens when the history setting grew. A
  failed fetch analyses the existing copy with a warning; nothing is deleted
  or re-cloned. The operating system may purge the cache, which costs one
  new download.
- **History** (the shell's menu, `codeInsight/historyDepth`): Files only,
  Last 300 commits (default) or Last 2,000 commits. Copies are cloned one
  commit deeper than the setting, because a commit whose parent is missing
  (the shallow boundary) is not counted.

History is built from the object store the way `git log --no-merges
--no-renames --numstat` reports it: newest first by commit date, merges
walked through but not counted, stopping at the history window, the commit
cap or the shallow boundary. Each commit's tree is diffed against its
parent's, only paths the engine analyses are kept, and added/deleted lines
come from a minimal (Myers) line diff; binary blobs count as `-`. The tests
compare the result field by field with the real `git log` on repositories
with merges, renames, binary files, a missing final newline and a shallow
clone. A rewrite of more than 4,096 changed lines in one file may count
slightly higher than git (iOS falls back to a full replacement there).
Complexity trends sample up to 16 commits that touched the file, oldest
first, as `complexityTrend` does.

One snapshot per repository is kept for ten minutes and shared by the three
pages, so opening Strata after Ember is instant. Analysis runs off the main
thread with the desktop's progress texts and is cancelled when the last page
showing that repository closes.

Limits: the phones' git engines reject submodule (gitlink) tree entries, so
a repository with submodules shows that error, as Clone to device does.
**Open file** opens the file in the app's file viewer at the top, because the
phone viewers have no line parameter yet.

### The pages

Each page takes a `CodeInsightContext` (snapshot, repository title, branch,
source label, `openFile`, `loadTrend`, `reanalyze`, `setSummary`); the
shell's status line ends with the page's summary, as the desktop's does.
Everything the desktop page shows is there, laid out for touch: chips for
tabs and views, pinch to zoom, drag to pan or rotate, tap to select (with a
haptic), bottom sheets with detents for details, and a list for everything
drawn on a canvas so VoiceOver and TalkBack users reach every fact. See the
phone sections of [ember.md](ember.md#on-phones),
[strata.md](strata.md#on-phones) and [synapse.md](synapse.md#on-phones).

Entry points: Home → Insights (Ember, Strata, Synapse), the repository
screen (iOS actions menu, Android hub tiles) and a local repository's More
menu, each shown only while the package is installed.
